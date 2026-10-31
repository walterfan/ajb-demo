"""Bounded PTY acceptance checks: no microphone, no real speaker output."""

import csv
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import wave


def main():
    root = Path(__file__).resolve().parents[1]
    binary = Path(sys.argv[1]).resolve(strict=True)
    if root not in binary.parents:
        raise ValueError("Pass the demo executable inside this project's build directory")
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 120, 0, 0))
    env = os.environ.copy()
    env["TERM"] = "xterm-256color"
    env["NO_COLOR"] = "1"
    run_root = root / "out" / ("pty-" + str(os.getpid()))
    source = Path(sys.argv[2]).resolve(strict=True) if len(sys.argv) > 2 else root / "assets/speech.wav"
    process = subprocess.Popen([str(binary), "--tui", "--ascii",
        "--input", str(source),
        "--output-dir", str(run_root)],
        stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True)
    transcript = bytearray()

    def drain(seconds=.4):
        deadline = time.monotonic() + seconds
        start = len(transcript)
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], .04)
            if readable:
                chunk = os.read(master, 65536)
                transcript.extend(chunk)
                if b"\x1b[6n" in chunk:
                    os.write(master, b"\x1b[1;1R")
                if len(transcript) > 8 * 1024 * 1024:
                    raise RuntimeError("PTY transcript exceeded its bound")
            if process.poll() is not None:
                break
        return transcript[start:].decode("utf-8", errors="replace")

    def send(value, wait=.4):
        os.write(master, value)
        return drain(wait)

    def assert_text(text):
        plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", transcript)
        if text.encode() not in plain:
            raise AssertionError("Missing TUI text: " + text + "; tail=" + repr(bytes(transcript[-1500:])))

    def wait_text(text, timeout=15):
        deadline = time.monotonic() + timeout
        while text.encode() not in re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", transcript):
            if process.poll() is not None or time.monotonic() >= deadline:
                assert_text(text)
            drain(.15)

    try:
        wait_text("Ready")
        send(b"-")  # 0.5x => visual-only; avoid speakers during the test.
        send(b"\r", .1)
        wait_text("run-")
        send(b"d")
        assert_text("Reference bypasses network/buffer")
        send(b"v")
        send(b"3")
        send(b".")
        assert_text("Visual step")
        send(b"n")
        send(b"d")
        assert_text("Delay(ms), last 5s")
        assert_text("N last arrival")
        assert_text("P=played-N+B")
        send(b"e")
        send(b"v")  # Delay wraps back to Overview
        send(b"\t" * 4)  # focus timeline; seek clears audio and stays paused
        send(b"\x1b[C")
        send(b"e")
        def snapshot_time():
            snapshot = max(run_root.glob("run-*/snapshot-*.txt"),
                           key=lambda path: path.stat().st_mtime_ns)
            return int(re.search(r"time_ns=(\d+)", snapshot.read_text()).group(1))
        forward = snapshot_time()
        send(b"\x1b[D")
        send(b"e")
        if forward - snapshot_time() != 1_000_000_000:
            raise AssertionError("Timeline arrows did not seek exactly one second")
        send(b"\t")  # focus the teaching checkbox
        send(b" ")
        send(b"3")  # restart adaptive, preserving enabled teaching
        wait_text("Teaching pause")
        send(b" ")  # disable teaching without invoking global transport
        send(b"v")
        assert_text("ordered queue")
        send(b"e")
        assert_text("Snapshot saved")
        send(b"?")
        assert_text("AJB Lab help")
        send(b"\x1b")
        # Apply unchanged valid parameters; every policy must be regenerated
        # in a new run while the first complete run remains on disk.
        before = set(run_root.glob("run-*/summary.csv"))
        send(b"p")
        assert_text("small ms (small fixed budget)")
        assert_text("initial ms (adaptive initial)")
        send(b"\t" * 10)
        send(b"\r", .2)
        deadline = time.monotonic() + 15
        while not set(run_root.glob("run-*/summary.csv")) - before:
            if time.monotonic() > deadline:
                raise AssertionError("Parameter apply did not produce a new complete run")
            drain(.2)
        if not before.issubset(set(run_root.glob("run-*/summary.csv"))):
            raise AssertionError("Rerun removed previous results")
        drain(.4)
        send(b"p")
        assert_text("Pending parameters")
        send(b"\t" * 2)  # seed input, after policy and scene
        send(b"q")  # must not invoke the global quit shortcut
        if process.poll() is not None:
            raise AssertionError("Text editing invoked the quit shortcut")
        send(b"\t" * 8)
        send(b"\r")
        wait_text("Use an integer within")
        send(b"\x1b")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        os.kill(process.pid, signal.SIGWINCH)
        drain(.4)
        assert_text("q quit")
        send(b"3")
        send(b".")
        transcript.clear()
        send(b"d")
        assert_text("Delay(ms), last 5s")
        assert_text("q quit")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 20, 60, 0, 0))
        os.kill(process.pid, signal.SIGWINCH)
        drain(.4)
        assert_text("Resize terminal")
        send(b"q")
        process.wait(timeout=5)
        if process.returncode != 0:
            raise AssertionError("TUI did not exit cleanly")
        restored = termios.tcgetattr(slave)
        if (restored[3] & (termios.ECHO | termios.ICANON)) != (original[3] & (termios.ECHO | termios.ICANON)):
            raise AssertionError("Terminal ECHO/ICANON was not restored")
        plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", transcript)
        if not plain.isascii():
            raise AssertionError("ASCII mode emitted non-ASCII display characters")
        # Use a short normalized result for bounded EOF checks at 80x24.
        short = run_root / "short.wav"
        rendered = next(run_root.glob("run-*/adaptive.wav"))
        with wave.open(str(rendered), "rb") as reader:
            with wave.open(str(short), "wb") as writer:
                writer.setparams(reader.getparams())
                writer.writeframes(reader.readframes(24000))
        individual = run_root / "individual"
        transcript.clear()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        process = subprocess.Popen([str(binary), "--tui", "--ascii",
            "--input", str(short), "--output-dir", str(individual),
            "--mode", "fixed", "--buffer-ms", "80"],
            stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True)
        wait_text("Ready")
        assert_text("Single test: fixed-80")
        send(b"-")  # keep every individual test silent

        def choose_policy(target):
            send(b"\r")  # open, move the radio focus, then confirm
            send(b"\x1b[H")
            send(b"\x1b[B" * target)
            send(b"\r")

        send(b"p")
        assert_text("Fixed custom")  # CLI mode/budget must populate the form
        choose_policy(3)
        transcript.clear()
        send(b"\x1b")  # cancel the pending adaptive choice
        assert_text("Single test: fixed-80")
        if list(individual.glob("run-*/summary.csv")):
            raise AssertionError("Cancelling a strategy selection started a run")

        def completed_run(before, expected):
            deadline = time.monotonic() + 15
            while not set(individual.glob("run-*/summary.csv")) - before:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError("Selected strategy did not complete preparation")
                drain(.15)
            added = set(individual.glob("run-*/summary.csv")) - before
            if len(added) != 1:
                raise AssertionError("One apply produced multiple runs")
            summary = added.pop()
            wait_text(summary.parent.name)
            with summary.open(encoding="utf-8") as stream:
                names = [row["policy"] for row in csv.DictReader(stream)]
            if names != expected:
                raise AssertionError("Wrong policies rendered: " + repr(names))
            if len(list(summary.parent.glob("*.wav"))) != len(expected):
                raise AssertionError("Single test exported other policy audio")
            send(b"e")
            snapshot = next(summary.parent.glob("snapshot-*.txt"))
            expected_track = expected[0] if len(expected) == 1 else "reference"
            if "policy=" + expected_track + " " not in snapshot.read_text():
                raise AssertionError("Playback did not start with the selected policy")
            if not before.issubset(set(individual.glob("run-*/summary.csv"))):
                raise AssertionError("Strategy change removed previous results")
            if len(expected) == 1:
                transcript.clear()
                send(b"\r")  # resume at the Start/Pause button
                wait_text("Finished; whole-run results")
            return summary

        transcript.clear()
        send(b"\r", .1)
        completed_run(set(), ["fixed-80"])
        for target, expected in [(1, ["fixed-20"]), (2, ["fixed-150"]),
                                 (3, ["adaptive"]), (4, ["fixed-80"]),
                                 (0, ["fixed-20", "fixed-150", "adaptive"])]:
            before = set(individual.glob("run-*/summary.csv"))
            send(b"p")
            choose_policy(target)
            if target == 4:
                send(b"\t" * 9)  # custom fixed budget
                send(b"\x1b[H" + b"\x1b[3~" * 12 + b"80")
                send(b"\t")
            else:
                send(b"\t" * 10)
            (run_root / "policy-selection.log").write_bytes(transcript)
            transcript.clear()
            send(b"\r", .1)
            completed_run(before, expected)
            if target == 3:
                transcript.clear()
                send(b"1")
                wait_text("Policy not in this run")
                send(b"3")
                send(b"e")
                latest = max(individual.glob("run-*/snapshot-*.txt"),
                             key=lambda path: path.stat().st_mtime_ns)
                if "policy=adaptive " not in latest.read_text():
                    raise AssertionError("Shortcut 3 did not select adaptive alone")
                before_cancel = set(individual.glob("run-*/summary.csv"))
                send(b"p")
                choose_policy(1)
                transcript.clear()
                send(b"\x1b")
                assert_text("Single test: adaptive")
                if set(individual.glob("run-*/summary.csv")) != before_cancel:
                    raise AssertionError("Cancel changed the completed experiment")
        # Numeric network controls stay usable on the compact terminal.
        def set_number(value):
            send(b"\x1b[H" + b"\x1b[3~" * 12 + str(value).encode())

        def assert_network_base(value):
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", transcript)
            if not re.search(rb"base delay ms \([^\r\n)]*\):\s+" + str(value).encode() + rb"\s", plain):
                raise AssertionError("Network base was not retained: " + str(value))

        before_network = set(individual.glob("run-*/summary.csv"))
        send(b"p")
        send(b"\x1bOQ")  # F2 opens Network and focuses base delay
        assert_text("base delay ms")
        assert_text("base delay ms (base one-way delay)")
        assert_text("burst jitter ms (burst background jitter)")
        set_number(900)  # phased maximum would be 900+20+120=1040
        send(b"\x1bOP")  # F1 Buffer
        transcript.clear()
        send(b"\x1bOQ")  # F2 preserves the pending base
        assert_network_base(900)
        send(b"\t" * 5)
        send(b"\r")
        wait_text("Network delay exceeds 1000ms")
        if set(individual.glob("run-*/summary.csv")) != before_network:
            raise AssertionError("Invalid network parameters started a run")
        send(b"\x1b")
        send(b"p")
        transcript.clear()
        send(b"\x1bOQ")
        assert_network_base(30)  # cancellation restored the current experiment
        # Choose constant, then fill all five network timing fields.
        send(b"\x1b[Z")  # Shift-Tab from base to the scene dropdown
        send(b"\r")
        send(b"\x1b[H")
        send(b"\r")
        send(b"\t")
        for value in [100, 40, 10, 60, 90]:
            set_number(value)
            send(b"\t")
        transcript.clear()
        send(b"\r", .1)
        summary = completed_run(before_network, ["fixed-20", "fixed-150", "adaptive"])
        metadata = (summary.parent / "experiment.txt").read_text()
        for setting in ["scenario=constant", "seed=42", "network_base_ms=100",
                        "jitter_ms=40", "burst_jitter_ms=10", "burst_extra_ms=60",
                        "reorder_extra_ms=90"]:
            if setting + "\n" not in metadata:
                raise AssertionError("Network setting not applied: " + setting)
        with (summary.parent / "network.csv").open(encoding="utf-8") as stream:
            for packet in csv.DictReader(stream):
                if int(packet["arrival_ns"]) - int(packet["send_ns"]) != 100_000_000:
                    raise AssertionError("TUI base delay did not drive actual arrivals")
        send(b"3")
        transcript.clear()
        send(b"d")
        wait_text("= 100.0ms")
        send(b"p")
        transcript.clear()
        send(b"\x1bOQ")
        assert_network_base(100)
        assert_text("Apply and rerun")
        assert_text("Esc cancel")
        # Tabs can select the Network page without function keys too.
        send(b"\x1b[Z" * 2)  # base -> scene -> form tabs
        send(b"\x1b[D")
        send(b"\x1b[C")
        send(b"\x1b")
        send(b"q")
        process.wait(timeout=5)
        if process.returncode != 0:
            raise AssertionError("Individual policy TUI did not exit cleanly")
        # A Chinese UI on a compact terminal must also restore terminal state
        # after Ctrl-C, not just after the normal quit action.
        transcript.clear()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        process = subprocess.Popen([str(binary), "--tui", "--input", str(source)],
            stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True)
        wait_text("就绪")
        send(b"p")
        assert_text("seed (随机种子，复现实验)")
        assert_text("min ms (自适应预算下限)")
        send(b"\x1bOQ")
        assert_text("base delay ms (基础单程网络延迟)")
        assert_text("reorder extra ms (乱序包附加延迟)")
        send(b"\x03")
        process.wait(timeout=5)
        restored = termios.tcgetattr(slave)
        if (restored[3] & (termios.ECHO | termios.ICANON)) != (original[3] & (termios.ECHO | termios.ICANON)):
            raise AssertionError("Ctrl-C did not restore terminal state")
        out = root / "out/pty-transcript.txt"
        out.write_text(re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", transcript.decode("utf-8", errors="replace")), encoding="utf-8")
        print("PTY smoke passed: guide, transport, policies, numeric network controls, resize, cleanup")
    finally:
        (root / "out").mkdir(exist_ok=True)
        (root / "out/pty-raw.log").write_bytes(transcript)
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
