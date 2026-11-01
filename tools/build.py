"""Argument-list-only build driver; never changes global Conan configuration."""

import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def capture(args):
    return run(args, text=True, stdout=subprocess.PIPE).stdout


def flag(name, default="ON"):
    value = os.environ.get(name, default).upper()
    if value not in ("ON", "OFF"):
        raise ValueError(f"{name} must be ON or OFF")
    return value == "ON"


def main():
    action = sys.argv[1] if len(sys.argv) == 2 else "help"
    if action == "help":
        print("make deps doctor build start compare test fixtures fetch-speech clean asan tsan doc web")
        print("INPUT=assets/real/mandarin.wav OUT=out SCENARIO=phased SEED=42 ARGS='...' ")
        print("MEDIA=OFF TUI=OFF TESTS=ON OFFLINE=ON PROFILE_HOST=default")
        return
    if action == "clean":
        for name in ("build", "out", "assets"):
            path = ROOT / name
            if path.is_symlink():
                raise ValueError(f"Refusing to clean symlink: {name}")
            if path.exists():
                shutil.rmtree(path)
        return
    media, tests = flag("MEDIA"), flag("TESTS")
    tui = media and flag("TUI")
    offline = flag("OFFLINE", "OFF")
    build_type = os.environ.get("BUILD_TYPE", "RelWithDebInfo")
    sanitizer = os.environ.get("SANITIZER", "none")
    if build_type not in ("Debug", "Release", "RelWithDebInfo"):
        raise ValueError("BUILD_TYPE must be Debug, Release or RelWithDebInfo")
    if sanitizer not in ("none", "address", "thread"):
        raise ValueError("SANITIZER must be none, address or thread")
    host = os.environ.get("PROFILE_HOST", "default")
    build_profile = os.environ.get("PROFILE_BUILD", "default")
    version = capture(["conan", "--version"]).strip()
    match = re.search(r"(\d+)\.(\d+)\.(\d+)", version)
    if not match or not ((2, 28) <= tuple(map(int, match.groups()[:2])) < (3, 0)):
        raise ValueError("Install Conan >=2.28,<3")
    profiles = capture(["conan", "profile", "show", "-pr:h", host,
                        "-pr:b", build_profile, "--format=json"])
    config = dict(media=media, tui=tui, tests=tests, build_type=build_type,
                  sanitizer=sanitizer, profiles=profiles)
    digest = hashlib.sha256(json.dumps(config, sort_keys=True).encode())
    for name in ("conanfile.py", "conan.lock"):
        digest.update((ROOT / name).read_bytes())
    identity = digest.hexdigest()[:16]
    folder = ROOT / "build" / f"{build_type}-{sanitizer}-{identity}"
    generators = folder / "generators"
    binary = folder / "cmake" / "ajb_demo"
    if action == "doctor":
        print(version)
        print("Host:", json.loads(profiles)["host"]["settings"])
        run(["cmake", "--version"])
        if media:
            modules = ["gstreamer-1.0", "gstreamer-app-1.0", "gstreamer-audio-1.0"]
            versions = [capture(["pkg-config", "--modversion", m]).strip()
                        for m in modules]
            prefixes = [capture(["pkg-config", "--variable=prefix", m]).strip()
                        for m in modules]
            if len(set(versions)) != 1 or len(set(prefixes)) != 1:
                raise ValueError("GStreamer core/app/audio must use one SDK")
            print("GStreamer", versions[0], "prefix", prefixes[0])
            print("System plugins:", capture([
                "pkg-config", "--variable=pluginsdir", "gstreamer-1.0"]).strip())
            for element in ("filesrc", "wavparse", "audioconvert", "audioresample",
                            "appsink", "appsrc", "wavenc", "filesink", "autoaudiosink"):
                run(["gst-inspect-1.0", element], stdout=subprocess.DEVNULL,
                    timeout=20)
                print(element + ": OK")
        return
    allowed = {"deps", "build", "test", "fixtures", "compare", "start", "web"}
    if action not in allowed:
        raise ValueError("Unknown build action")
    folder.mkdir(parents=True, exist_ok=True)
    stamp = folder / "dependency-stamp"
    if not stamp.exists() or not (generators / "conan_toolchain.cmake").exists():
        cmd = ["conan", "install", ".", "-of", str(folder), "--lockfile=conan.lock",
               "-pr:h", host, "-pr:b", build_profile,
               "-s:h", "compiler.cppstd=20", "-s:h", "build_type=" + build_type,
               "-o", "&:with_tui=" + str(tui),
               "-o", "&:with_tests=" + str(tests),
               "-o", "&:with_media=" + str(media), "--build=missing"]
        if offline:
            cmd.append("--no-remote")
        run(cmd)
        stamp.write_text(identity + "\n", encoding="utf-8")
    if action == "deps":
        print("Dependencies:", generators)
        return
    cmd = ["cmake", "-S", ".", "-B", str(folder / "cmake"),
           "-DCMAKE_TOOLCHAIN_FILE=" + str(generators / "conan_toolchain.cmake"),
           "-DCMAKE_BUILD_TYPE=" + build_type,
           "-DAJB_ENABLE_GSTREAMER=" + ("ON" if media else "OFF"),
           "-DAJB_ENABLE_TUI=" + ("ON" if tui else "OFF"),
           "-DAJB_BUILD_TESTS=" + ("ON" if tests else "OFF"),
           "-DAJB_ENABLE_ASAN=" + ("ON" if sanitizer == "address" else "OFF"),
           "-DAJB_ENABLE_TSAN=" + ("ON" if sanitizer == "thread" else "OFF")]
    run(cmd)
    jobs = int(os.environ.get("JOBS", "8"))
    if not 1 <= jobs <= 128:
        raise ValueError("JOBS must be between 1 and 128")
    run(["cmake", "--build", str(folder / "cmake"), "-j", str(jobs)])
    env = os.environ.copy()
    if sanitizer == "address":
        env.setdefault("ASAN_OPTIONS", "detect_container_overflow=0:detect_stack_use_after_return=1")
    # Read Conan's generated data via its documented environment API output;
    # the default static FTXUI/GTest graph has no runtime shared libraries.
    # Shared-library variants use Conan's script in the same shell as the command.
    def execute(args):
        activation = generators / "conanrun.sh"
        if activation.exists():
            run(["/bin/sh", "-c", '. "$1"; shift; cd "$1"; shift; exec "$@"',
                 "ajb-run", str(activation), str(ROOT), *map(str, args)],
                env=env, cwd=generators)
        else:
            run(list(map(str, args)), env=env)
    if action == "build":
        print("Executable:", binary if media else folder / "cmake" / "ajb_tests")
    elif action == "test":
        if not tests:
            raise ValueError("make test requires TESTS=ON")
        execute(["ctest", "--test-dir", folder / "cmake", "--output-on-failure"])
    else:
        if not media:
            raise ValueError("This action requires MEDIA=ON")
        source = os.environ.get("INPUT") or "assets/real/mandarin.wav"
        if action == "web":
            execute([sys.executable, ROOT / "tools/web_server.py", "--binary", binary,
                     "--input", source, "--output-dir",
                     Path(os.environ.get("OUT", "out")) / "web", "--port",
                     os.environ.get("WEB_PORT", "8765")])
            return
        fixture = ROOT / "assets" / "speech.wav"
        if action == "fixtures":
            fixture.parent.mkdir(exist_ok=True)
            execute([binary, "--generate-fixture", fixture])
        if action != "fixtures":
            extras = shlex.split(os.environ.get("ARGS", ""))
            ui_args = [] if any(x in extras for x in ("--ui", "--tui", "--no-tui")) else [
                "--ui", "auto" if action == "start" else "plain"]
            execute([binary, "--input", source,
                     "--output-dir", os.environ.get("OUT", "out"),
                     "--scenario", os.environ.get("SCENARIO", "phased"),
                     "--seed", os.environ.get("SEED", "42"),
                     *ui_args, *extras])


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("ajb build:", error, file=sys.stderr)
        sys.exit(1)
