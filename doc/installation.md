# 安装与构建

项目要求 C++20、uv、CMake ≥3.20，以及 GStreamer ≥1.20。
Python 工具由 `pyproject.toml` 和 `uv.lock` 管理，默认 Python 3.13，Conan 固定为 2.28.1。
Conan 固定 `ftxui/7.0.3`（MIT）和 `gtest/1.18.0`（BSD-3-Clause），来源为
ConanCenter 配方或用户配置的镜像；真实生成的 `conan.lock` 锁定版本和 recipe
revision。GStreamer 使用下文的完整系统 SDK，不混用 Conan GStreamer core。

```sh
cd ajb-demo
uv sync --locked                 # 创建 .venv 并安装锁定的 Python 工具
# 仅在尚无 default profile 时执行：
uv run --locked conan profile detect
make deps                       # 按锁文件准备依赖
make doctor                     # 只读环境检查
make build
make start                      # 交互终端中的导览
make compare                    # 默认 mandarin.wav，不打开声卡或全屏终端
make test
make test MEDIA=OFF TUI=OFF      # 只构建核心测试，不要求 GStreamer/FTXUI
make test MEDIA=OFF TUI=OFF OFFLINE=ON  # 已缓存依赖的离线验证
```

`make start` 和 `make compare` 默认使用仓库中的 `assets/real/mandarin.wav`，可通过
`INPUT=/path/to/speech.wav` 覆盖。若样本已删除，执行 `make fetch-speech` 重新获取。

`PROFILE_HOST`/`PROFILE_BUILD` 默认使用现有 `default`；缺失时先自行准备 profile，
构建不会覆盖用户配置。可用 `BUILD_TYPE=Debug`、`JOBS=8`、`TUI=OFF`、`TESTS=OFF`
选择配置。不同配置保存在 `build/` 的独立子目录，CMake 显式使用 Conan toolchain，
不依赖 generated presets。`make asan` 与 `make tsan` 使用独立 Debug 目录；
第三方依赖默认是未插桩的 Conan 库，报告中区分应用和第三方插桩范围。
`make clean` 只清除演示的 build/out/assets，不动 Conan 全局缓存或锁文件。

Conan 锁文件不锁定编译器和系统媒体 SDK。当前验证环境为 macOS arm64、
AppleClang 21、Conan 2.28.1、GStreamer 1.28.8；用 `make doctor` 查看实际环境。

## macOS：安装 GStreamer

Use Homebrew to install the development libraries, tools, and plugins:

```sh
brew install gstreamer pkgconf
```

Homebrew bundles the common GStreamer plugin sets in the `gstreamer` formula;
separate `gst-plugins-base` and `gst-plugins-good` installations are unnecessary.
See the [Homebrew formula](https://formulae.brew.sh/formula/gstreamer).

Use the same Homebrew installation for GStreamer core, app/audio libraries,
and runtime plugins. The demo's Conan 2 dependency plan covers FTXUI and
GoogleTest; GStreamer uses this complete system installation.

## Verify the installation

Check the executable and development libraries:

```sh
gst-launch-1.0 --version

pkg-config --modversion \
  gstreamer-1.0 \
  gstreamer-app-1.0 \
  gstreamer-audio-1.0
```

The `pkg-config` command should print a version for each of the three libraries.
If it cannot find them, add GStreamer's package metadata directory to the
current shell's search path, then retry:

```sh
export PKG_CONFIG_PATH="$(brew --prefix gstreamer)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
```

Check the elements needed by the demo:

```sh
for element in appsrc appsink wavparse wavenc \
               audioconvert audioresample autoaudiosink; do
  if gst-inspect-1.0 "$element" >/dev/null; then
    echo "$element: OK"
  else
    echo "$element: MISSING"
  fi
done
```

Each element should report `OK`. Element availability and audio-device
availability are separate checks.

To verify speaker output, this command plays a short test tone:

```sh
gst-launch-1.0 audiotestsrc num-buffers=50 \
  ! audioconvert ! audioresample ! autoaudiosink
```

For a silent processing check without an audio device, use:

```sh
gst-launch-1.0 -q audiotestsrc num-buffers=5 \
  ! audioconvert ! audioresample ! fakesink
```

## Troubleshooting plugin-scanner errors on macOS

During startup, including `gst-launch-1.0 --version`, the plugin scanner may
report:

```text
Failed to load shared library 'libgobject-2.0.0.dylib'
Failed to load shared library 'libglib-2.0.0.dylib'
can't find gi.repository.Gst
```

We reproduced these messages on Apple Silicon with Homebrew GStreamer 1.28.8.
Both libraries were installed in `/opt/homebrew/lib`, but GI introspection
could not locate them by filename while initializing the Python plugin.

If the current command is stuck, stop it with **Ctrl-C**, then set the fallback
library search path and retry:

```sh
export DYLD_FALLBACK_LIBRARY_PATH="$(brew --prefix)/lib:/usr/local/lib:/usr/lib"

gst-launch-1.0 --version
```

In the tested installation, this printed:

```text
gst-launch-1.0 version 1.28.8
GStreamer 1.28.8
```

The same setting also passed all seven element checks and the silent pipeline
above. The version printed on another installation may differ.

This export affects commands started from the current shell. It fixes the
observed library-loading errors; those errors alone do not establish the cause
of a persistent hang. In our diagnostic run, the unmodified command also
eventually completed.

GStreamer's [runtime environment reference](https://gstreamer.freedesktop.org/documentation/gstreamer/running.html)
documents plugin search paths, registry caches, and debug logging for further
diagnosis.
