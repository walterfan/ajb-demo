# Adaptive Jitter Buffer Demo

一个独立的 C++20 自适应抖动缓冲实验项目。在同一条网络轨迹下比较小固定、大固定
与自适应缓冲，通过中文 Web GUI、TUI、音频试听和 WAV/CSV 导出观察延迟与播放连续性的取舍。

## 快速开始

需要 C++20 编译器、uv、CMake ≥3.20 和 GStreamer ≥1.20。
Python 工具依赖由 uv 管理；FTXUI 和 GoogleTest 由 Conan 管理，详见[安装与构建](doc/installation.md)。

```sh
git clone git@github.com:walterfan/ajb-demo.git
cd ajb-demo
uv sync --locked
# 首次使用 Conan、尚无 default profile 时执行：
uv run --locked conan profile detect
make deps
make build
make web                        # Web GUI：http://127.0.0.1:8765
make start                      # 中文 TUI，也使用同一默认语音
make compare                    # 使用同一默认语音离线对比
make test
```

Web GUI 支持参数重跑、四轨试听、曲线定位和 WAV/CSV 下载，见 [Web 操作指南](doc/web-gui.md)。
命令行实验写入新的 `out/run-N/`。TUI 中按 `p` 调整策略和网络参数，`d` 查看延迟，
Space 暂停，`n` 逐事件，`q` 退出。默认输入为 `assets/real/mandarin.wav`，可用
`INPUT=/path/to/speech.wav` 替换；无声卡也可运行 `make compare`。
默认基础延迟 100ms、抖动上限 120ms，5–15 秒包含抖动和突发阶段；按 `1`、`2`、`3`
切换小固定、大固定和自适应缓冲，比较卡顿与等待。

## 文档

- [文档首页](doc/index.md)
- [安装与构建](doc/installation.md) · [实验与 TUI 操作](doc/usage.md)
- [自适应抖动缓冲设计](doc/design.md) · [代码实现](doc/implementation.md)
- [WSOLA、PLC 与静音帧裁剪](doc/audio-processing.md) · [PLC / Rebuffer 对照测试](doc/underrun.md)
- [输出与指标](doc/metrics.md) · [测试](doc/testing.md) · [验收记录](doc/validation.md)
- [参考资料](doc/references.md) · [构建 Sphinx/MyST 文档](doc/building.md)

```sh
make doc
```

生成的文档入口为 `doc/_build/html/index.html`。

## 许可

代码使用 [Apache-2.0](LICENSE)；真实语音样本来源及许可见[音频署名](assets/real/ATTRIBUTION.md)。
