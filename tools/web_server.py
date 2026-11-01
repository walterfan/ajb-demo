"""Loopback-only browser interface to the existing C++ experiment renderer."""

import argparse
import array
import asyncio
from contextlib import asynccontextmanager
import csv
import math
import os
from pathlib import Path
import re
import secrets
import sys
from typing import Literal
import wave

from fastapi import FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, ConfigDict, Field, model_validator
from starlette.middleware.trustedhost import TrustedHostMiddleware
import uvicorn

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
OPTIONS = {
    "scenario": "--scenario", "seed": "--seed",
    "underrun_policy": "--underrun-policy",
    "rebuffer_resume_ms": "--rebuffer-resume-ms",
    "rebuffer_timeout_ms": "--rebuffer-timeout-ms",
    "network_base_ms": "--network-delay-ms", "jitter_ms": "--jitter-ms",
    "burst_jitter_ms": "--burst-jitter-ms", "burst_extra_ms": "--burst-delay-ms",
    "reorder_extra_ms": "--reorder-delay-ms", "small_ms": "--small-buffer-ms",
    "large_ms": "--large-buffer-ms", "min_ms": "--min-delay-ms",
    "initial_ms": "--initial-delay-ms", "max_ms": "--max-delay-ms",
}
TICK_COLUMNS = ["time", "target", "level", "span", "advance", "concealed",
                "action", "network", "wait", "playout", "source", "plc_ms", "rebuffer_ms"]


class ExperimentConfig(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    scenario: Literal["constant", "jitter", "burst", "reorder", "loss", "phased"] = "phased"
    underrun_policy: Literal["plc", "rebuffer"] = "plc"
    rebuffer_resume_ms: int = Field(default=100, ge=40, le=500, multiple_of=10)
    rebuffer_timeout_ms: int = Field(default=300, ge=10, le=2000, multiple_of=10)
    seed: int = Field(default=42, ge=0, le=4294967295)
    network_base_ms: int = Field(default=100, ge=0, le=1000)
    jitter_ms: int = Field(default=120, ge=0, le=1000, multiple_of=10)
    burst_jitter_ms: int = Field(default=30, ge=0, le=1000, multiple_of=10)
    burst_extra_ms: int = Field(default=160, ge=0, le=1000)
    reorder_extra_ms: int = Field(default=100, ge=0, le=1000)
    small_ms: int = Field(default=20, ge=0, le=460, multiple_of=10)
    large_ms: int = Field(default=150, ge=0, le=460, multiple_of=10)
    min_ms: int = Field(default=40, ge=0, le=460, multiple_of=10)
    initial_ms: int = Field(default=60, ge=0, le=460, multiple_of=10)
    max_ms: int = Field(default=200, ge=0, le=460, multiple_of=10)

    @model_validator(mode="after")
    def validate_relationships(self):
        if self.small_ms >= self.large_ms:
            raise ValueError("小固定缓冲必须小于大固定缓冲")
        if not self.min_ms <= self.initial_ms <= self.max_ms:
            raise ValueError("自适应预算须满足：下限 ≤ 初始值 ≤ 上限")
        extra = {"jitter": self.jitter_ms,
                 "burst": self.burst_jitter_ms + self.burst_extra_ms,
                 "reorder": self.reorder_extra_ms,
                 "phased": max(self.jitter_ms, self.burst_jitter_ms + self.burst_extra_ms)}
        if self.network_base_ms + extra.get(self.scenario, 0) > 1000:
            raise ValueError("当前场景的基础延迟与最大附加延迟之和不能超过 1000ms")
        return self


def direct_path(value: Path, *, exists: bool = True) -> Path:
    if ".." in value.parts:
        raise ValueError("Use a direct path without '..'")
    value = value.absolute()
    for part in (value, *value.parents):
        if part.is_symlink():
            raise ValueError("Symbolic links are not accepted")
    if exists and not value.is_file():
        raise ValueError("Required local file is missing")
    return value


def source_info(source: Path) -> dict:
    if source.stat().st_size > 128 * 1024 * 1024:
        raise ValueError("WAV input exceeds 128MiB")
    with wave.open(str(source), "rb") as reader:
        duration = reader.getnframes() / reader.getframerate()
        if not 0 < duration <= 300:
            raise ValueError("WAV duration must be within 0..300s")
        peaks = []
        if reader.getsampwidth() == 2:
            chunk = max(1, math.ceil(reader.getnframes() / 800))
            for _ in range(800):
                data = reader.readframes(chunk)
                if not data:
                    break
                samples = array.array("h", data)
                if sys.byteorder != "little":
                    samples.byteswap()
                peaks.append(round(max(abs(n) for n in samples) / 32768, 4))
        return {"name": source.name, "duration": duration,
                "waveform": peaks, "sample_rate": reader.getframerate()}


def number(value: str) -> float | None:
    result = float(value)
    return result if math.isfinite(result) else None


def result_data(directory: Path) -> tuple[list, dict]:
    with (directory / "summary.csv").open(encoding="utf-8") as stream:
        summaries = list(csv.DictReader(stream))
    if len(summaries) != 3:
        raise ValueError("Expected three comparison policies")
    tracks, files = [], {}
    for summary in summaries:
        name = summary.pop("policy")
        if not re.fullmatch(r"fixed-\d{1,3}|adaptive", name):
            raise ValueError("Invalid result policy")
        ticks = []
        with (directory / (name + ".csv")).open(encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                if len(ticks) >= 31000:
                    raise ValueError("Too many output ticks")
                ticks.append([
                    int(row["output_ns"]) / 1e9, float(row["target_level_ms"]),
                    float(row["continuous_level_ms"]), float(row["span_ms"]),
                    int(row["consumed_samples"]), int(row["concealed_samples"]), row["action"],
                    number(row["played_network_ms"]), number(row["buffer_wait_ms"]),
                    number(row["packet_to_output_ms"]), int(row["source_sample"]),
                    float(row["plc_total_ms"]), float(row["rebuffer_total_ms"]),
                ])
        tracks.append({"id": name, "summary": {k: number(v) for k, v in summary.items()},
                       "ticks": ticks, "events": [], "events_truncated": False})
        for suffix in ("wav", "csv"):
            filename = name + "." + suffix
            files[filename] = direct_path(directory / filename)
    by_name = {track["id"]: track for track in tracks}
    with (directory / "events.csv").open(encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["kind"] not in {"late", "target", "duplicate", "overflow", "rebuffer_start", "rebuffer_end"}:
                continue
            events = by_name[row["policy"]]["events"]
            if len(events) < 5000:
                events.append({"time": int(row["time_ns"]) / 1e9,
                               "kind": row["kind"], "reason": row["reason"],
                               "packet": int(row["packet"]),
                               "target": int(row["target_budget_ms"])})
            else:
                by_name[row["policy"]]["events_truncated"] = True
    for filename in ("summary.csv", "network.csv", "events.csv", "experiment.txt"):
        files[filename] = direct_path(directory / filename)
    return tracks, files


def create_app(binary: Path, source: Path, output: Path) -> FastAPI:
    binary, source = direct_path(binary), direct_path(source)
    if not binary.is_relative_to(ROOT / "build") or not os.access(binary, os.X_OK):
        raise ValueError("Use an executable inside the project's build directory")
    output = direct_path(output, exists=False)
    output.mkdir(parents=True, exist_ok=True)
    info = source_info(source)
    jobs: dict[str, dict] = {}
    tasks: set[asyncio.Task] = set()

    @asynccontextmanager
    async def lifespan(_app):
        yield
        for task in list(tasks):
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    app = FastAPI(lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=["127.0.0.1", "localhost"])

    @app.middleware("http")
    async def local_requests(request: Request, call_next):
        if request.headers.get("sec-fetch-site") == "cross-site" and request.url.path != "/":
            return JSONResponse({"detail": "请使用本地实验页面"}, status_code=403)
        if request.method == "POST":
            if (request.headers.get("origin") != str(request.base_url).rstrip("/")
                    or request.headers.get("x-ajb-request") != "1"):
                return JSONResponse({"detail": "请从本地实验页面发起请求"}, status_code=403)
            if request.headers.get("content-type", "").split(";")[0] != "application/json":
                return JSONResponse({"detail": "需要 JSON 参数"}, status_code=415)
            length = request.headers.get("content-length", "")
            if not length.isdecimal() or not 0 < int(length) <= 4096:
                return JSONResponse({"detail": "参数大小超限"}, status_code=413)
            if len(await request.body()) > 4096:
                return JSONResponse({"detail": "参数大小超限"}, status_code=413)
        response = await call_next(request)
        response.headers.update({
            "Content-Security-Policy": "default-src 'self'; script-src 'self'; style-src 'self'; "
                                       "media-src 'self'; img-src 'self'; connect-src 'self'; "
                                       "object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
            "X-Content-Type-Options": "nosniff", "Referrer-Policy": "no-referrer",
            "Cache-Control": "no-store",
        })
        return response

    @app.exception_handler(RequestValidationError)
    async def invalid_config(_request, error):
        messages = [str(item["msg"]).removeprefix("Value error, ") for item in error.errors()]
        return JSONResponse({"detail": "；".join(messages[:3])}, status_code=422)

    async def render(job_id: str, config: ExperimentConfig):
        job = jobs[job_id]
        destination = output / job_id
        process = None
        try:
            command = [str(binary), "--input", str(source), "--output-dir", str(destination),
                       "--no-tui", "--mode", "compare"]
            for key, value in config.model_dump().items():
                command.extend([OPTIONS[key], str(value)])
            process = await asyncio.create_subprocess_exec(
                *command, cwd=ROOT, stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL)
            await asyncio.wait_for(process.wait(), timeout=120)
            if process.returncode:
                raise RuntimeError("Renderer failed")
            directory = direct_path(destination, exists=False) / "run-1"
            tracks, files = await asyncio.to_thread(result_data, directory)
            job.update(status="ready", tracks=tracks, files=files)
        except asyncio.CancelledError:
            if process and process.returncode is None:
                process.kill()
                await process.wait()
            raise
        except (OSError, ValueError, RuntimeError, asyncio.TimeoutError, csv.Error, KeyError):
            if process and process.returncode is None:
                process.kill()
                await process.wait()
            job.update(status="error", message="实验生成失败；请在终端执行 make compare 检查音频输入和输出权限。")

    @app.get("/")
    async def index():
        return FileResponse(WEB / "index.html")

    @app.get("/app.js")
    async def javascript():
        return FileResponse(WEB / "app.js", media_type="text/javascript")

    @app.get("/styles.css")
    async def stylesheet():
        return FileResponse(WEB / "styles.css", media_type="text/css")

    @app.get("/icon.svg")
    async def icon():
        return FileResponse(WEB / "icon.svg", media_type="image/svg+xml")

    @app.get("/api/config")
    async def configuration():
        return {"defaults": ExperimentConfig().model_dump(), "source": info,
                "columns": TICK_COLUMNS}

    @app.get("/api/source.wav")
    async def source_audio():
        return FileResponse(source, media_type="audio/wav")

    @app.post("/api/runs", status_code=202)
    async def start_run(config: ExperimentConfig):
        if tasks:
            raise HTTPException(409, "已有实验正在生成，请稍候")
        while len(jobs) >= 8:
            del jobs[next(iter(jobs))]
        job_id = secrets.token_hex(12)
        jobs[job_id] = {"id": job_id, "status": "rendering", "config": config.model_dump()}
        task = asyncio.create_task(render(job_id, config))
        tasks.add(task)
        task.add_done_callback(tasks.discard)
        return {"id": job_id, "status": "rendering"}

    @app.get("/api/runs/{job_id}")
    async def get_run(job_id: str):
        job = jobs.get(job_id)
        if not job:
            raise HTTPException(404, "实验已过期，请重新运行")
        return {key: value for key, value in job.items() if key != "files"}

    @app.get("/api/runs/{job_id}/files/{filename}")
    async def download(job_id: str, filename: str):
        job = jobs.get(job_id)
        if not job or job["status"] != "ready" or filename not in job["files"]:
            raise HTTPException(404, "文件不存在")
        path = direct_path(job["files"][filename])
        media_type = "audio/wav" if path.suffix == ".wav" else "text/plain"
        return FileResponse(path, media_type=media_type)

    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--input", type=Path, default=ROOT / "assets/real/mandarin.wav")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "out/web")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("port must be in 1024..65535")
    app = create_app(args.binary, args.input, args.output_dir)
    print(f"AJB Web GUI: http://127.0.0.1:{args.port}", flush=True)
    uvicorn.run(app, host="127.0.0.1", port=args.port, access_log=False)


if __name__ == "__main__":
    main()
