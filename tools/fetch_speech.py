"""Fetch a pinned human Mandarin reading and keep one continuous 45s passage."""

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
URL = ("https://archive.org/download/chao_hua_si_she_jl_librivox/"
       "chaohuasishe_00_lu_64kb.mp3")
SHA256 = "ddedebd2037ce78e0c6533cd55903473c10bdd0bb19fc72eaf8645af92346240"
MAX_BYTES = 8 * 1024 * 1024


def checksum(path):
    if not 0 < path.stat().st_size <= MAX_BYTES:
        raise ValueError("Speech source must be nonempty and at most 8MiB")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if len(sys.argv) != 1:
        raise ValueError("Use make fetch-speech without arguments")
    folder = ROOT / "assets" / "real"
    for path in [ROOT / "assets", folder]:
        if path.is_symlink():
            raise ValueError("Refusing a symlink in the speech asset directory")
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / "chaohuasishe_00_lu_64kb.mp3"
    output = folder / "mandarin.wav"
    attribution = folder / "ATTRIBUTION.md"
    for path in [source, output, attribution]:
        if path.is_symlink():
            raise ValueError("Refusing a symlink for a speech asset")
    with tempfile.TemporaryDirectory(prefix="fetch-", dir=folder) as temporary:
        temporary = Path(temporary)
        if not source.exists():
            download = temporary / "source.mp3"
            subprocess.run([
                "curl", "--fail", "--silent", "--show-error", "--location",
                "--proto", "=https", "--proto-redir", "=https",
                "--connect-timeout", "10", "--max-time", "60", "--retry", "1",
                "--max-filesize", str(MAX_BYTES), "--output", str(download), URL,
            ], check=True, timeout=130)
            if checksum(download) != SHA256:
                raise ValueError("Downloaded speech checksum mismatch; source was not installed")
            download.replace(source)
        if checksum(source) != SHA256:
            raise ValueError("Cached speech checksum mismatch; remove the source MP3 and retry")
        converted = temporary / "mandarin.wav"
        subprocess.run([
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-ss", "30", "-i", str(source), "-t", "45", "-map", "0:a:0",
            "-vn", "-sn", "-dn", "-ac", "1", "-ar", "48000",
            "-c:a", "pcm_s16le", "-map_metadata", "-1", "-fflags", "+bitexact",
            "-flags:a", "+bitexact", str(converted),
        ], check=True, timeout=60)
        with wave.open(str(converted), "rb") as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(),
                    audio.getnframes()) != (1, 2, 48000, 45 * 48000):
                raise ValueError("Decoded speech must be exactly 45s, 48kHz mono PCM16")
        digest = checksum(converted)
        note = temporary / "ATTRIBUTION.md"
        note.write_text(
            "# 连贯真人语音样本\n\n"
            "作品：鲁迅《朝花夕拾·小引》；朗读：Jing Li；语言：中文。\n"
            "来源：https://librivox.org/chao-hua-si-she-by-lu-xun/\n"
            "存档：https://archive.org/details/chao_hua_si_she_jl_librivox\n"
            "许可来源标注：LibriVox recordings are Public Domain in the USA；"
            "Internet Archive 标注 Public Domain Mark 1.0。\n"
            "许可说明：https://librivox.org/pages/public-domain/\n\n"
            f"下载：{URL}\n源文件 SHA-256：{SHA256}\n"
            "连续截取区间：[30, 75)秒；时长45秒，保留自然停顿。\n"
            "处理：解码、单声道下混、48kHz重采样、S16LE转换、移除媒体元数据。\n"
            f"mandarin.wav SHA-256：{digest}\n",
            encoding="utf-8",
        )
        converted.replace(output)
        note.replace(attribution)
    print("Ready: assets/real/mandarin.wav (45s, 48kHz mono PCM16; verified source)")
    print("Run: make start INPUT=assets/real/mandarin.wav")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, wave.Error, subprocess.SubprocessError) as error:
        print("fetch-speech:", error, file=sys.stderr)
        sys.exit(1)
