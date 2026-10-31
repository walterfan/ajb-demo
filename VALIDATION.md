# 演示验收记录

2026-10-31，macOS arm64，AppleClang 21，CMake 4.4.3，Conan 2.28.1。
系统媒体来自 Homebrew `gstreamer 1.28.8_1`：core/app/audio 均为 1.28.8，
prefix 为 `/opt/homebrew/Cellar/gstreamer/1.28.8_1`，插件目录为其
`lib/gstreamer-1.0`。Conan 使用现有 default profiles，host C++20；
FTXUI 7.0.3、GoogleTest 1.18.0 的确切 recipe revisions 见 `conan.lock`。

## 默认场景结果

固定 seed 的 30 秒合成样本，`phased`，seed=42，预算使用默认配置。
以下数值来自 `make compare` 的 `summary.csv`；延迟包含共同 40ms 前视，
设备声学延迟未测量。PLC 是静音补偿，未作主观音质评分。

| 策略 | 启动 ms | 平均等待 ms | P95 等待 ms | PLC ms | 迟到包 |
| --- | ---: | ---: | ---: | ---: | ---: |
| fixed-20 | 90 | 59.5227 | 69.3542 | 2050 | 205 |
| fixed-150 | 220 | 182.9429 | 199.3125 | 0 | 0 |
| adaptive | 130 | 100.9800 | 193.9792 | 159.5 | 7 |

`constant`/seed=42 的同长度实验：三种策略 PLC、迟到、溢出均为零；平均等待
分别为 64.9896、194.9896、66.3929ms（末位以重新生成的 CSV 为准）。
自适应在默认 phased 轨迹下补偿少于 fixed-20，平均等待低于 fixed-150。

```sh
make compare SCENARIO=phased SEED=42 OUT=out/accept-phased OFFLINE=ON
make compare SCENARIO=constant SEED=42 OUT=out/accept-constant OFFLINE=ON
make test OFFLINE=ON
```

## 已执行验证

真人连贯语音补充：Jing Li 朗读鲁迅《朝花夕拾·小引》，来源及许可见
[LibriVox](https://librivox.org/chao-hua-si-she-by-lu-xun/)和
[Internet Archive](https://archive.org/details/chao_hua_si_she_jl_librivox)。
从单个原录音连续取[30,75)秒，输出45.000秒、48kHz、单声道、S16LE，2160000样本，
峰值12898、RMS -31.06dBFS。保留自然停顿，未拼接或循环。
`make fetch-speech`使用本机FFmpeg 9.0.2，已验证实际下载、SHA-256校验和缓存转换，
两次生成的WAV一致。临时副本验证损坏、超限及符号链接缓存被拒绝，已有WAV保留。
以完整45秒真人样本运行的PTY检查通过，覆盖回放、单步、参数重跑、五种策略选择、
单独测试结束和终端清理；该自动检查保持静音，不作主观听感评分。

以该真人音频运行phased/seed=42，结果如下：

| 策略 | 平均等待 ms | PLC ms | 迟到包 |
| --- | ---: | ---: | ---: |
| fixed-20 | 61.4320 | 2050 | 205 |
| fixed-150 | 186.9585 | 0 | 0 |
| adaptive | 85.6601 | 159.5 | 7 |

```sh
make fetch-speech
make compare INPUT=assets/real/mandarin.wav SCENARIO=phased SEED=42 OUT=out/mandarin-phased
make start INPUT=assets/real/mandarin.wav
```

- 完整演示 38 项测试通过；核心模式 26 项测试通过。覆盖虚拟时钟、容量、
  PLC、WSOLA 音高/消费边界、指标、事件前缀、WAV 转换与取消、参数和输出文件。
- 延迟页版本执行 `make asan TUI=OFF OFFLINE=ON`：34 项通过；ASan/UBSan覆盖核心、回放状态、
  文件 I/O 和无声卡媒体。`make tsan OFFLINE=ON`：34 项通过，完整 TUI 可编译。
  最后媒体警告处理的改动另行复验媒体测试。GStreamer、FTXUI、GTest 为未插桩
  系统/Conan 库；这些结果不声明第三方库内部已获得完整 sanitizer 覆盖。
- PTY 检查包括总览/机制/结果、轨道切换、暂停、10ms/事件单步、时间定位、
  真实事件教学暂停、快照、参数重跑、输入快捷键隔离、120×36/80×24/过小终端、
  ASCII/中文、q/Ctrl-C 和终端 ECHO/ICANON 恢复。完整 TSan TUI 也执行 PTY 检查。
- 使用仓库真实语音 `assets/real/speech.wav` 的前三秒，完成实际 `make start`
  有声四轨导览、手动重播/暂停/单步/导出和 seed 改为43后重跑；另验证纯文本
  四轨试听。短录音用于操作和设备验证，恶化/恢复效果由上述30秒实验验证。
- 重定向 `make start` 自动完成无声卡离线渲染，未输出 ANSI 或等待输入；显式
  不可用 TUI 给出 `--no-tui` 修正方法。
- 仅在测试子进程禁用 osxaudiosink/oss4sink/decklinkaudiosink 的 rank，验证设备
  不可用时明确可视降级、继续单步及退出；文本试听明确报错且保留 WAV/CSV。
  未修改全局插件配置。设备失败不改变已经生成的网络统计。
- 从临时干净源码副本执行完整 `make test`、默认 `make compare`、核心模式测试，
  全部 `OFFLINE=ON`；锁文件未改变，无源码 UserPresets。此前首次准备已真实
  构建 Conan 依赖，验证了完整/UI关闭/测试关闭/核心图及后续缓存命中。
- 缺失 profile 明确失败；隔离临时 GStreamer 注册表模拟缺失插件，doctor 明确
  指出 filesrc 缺失；临时副本验证 clean 仅删除本演示生成目录。
- 根工程 `make verify` 通过：62 项原测试、样本生成及端到端演示。根工程不依赖
  Conan 或 GStreamer；根构建未更改。

本次补充单策略选择：新增集成用例验证 fixed-20、fixed-150、fixed-80、adaptive
每轮只导出一份策略结果，且与相同配置的对比结果一致；元数据记录模式及固定
预算。扩展 PTY 用例在80×24终端验证五种选择、CLI固定80ms保留、取消待选策略、
单策略直接播放及EOF停止、adaptive仍使用快捷键3、未运行策略的提示、切回对比
和旧结果保留。ASan/UBSan、TSan的 Options/Experiment 测试均通过；完整 TSan
TUI 也通过上述五种策略、单独结束和取消/切换的 PTY 验证。

延迟页补充：按d查看最近5秒N/B/P曲线、包网络延迟及当前输出块均值。新增5项
测试验证真实send时刻、缺失块NaN、WSOLA首次贡献计权、到达/输出事件前缀、
五秒窗口及CSV一致性。34项完整测试与独立ASan/UBSan、TSan均通过。
普通及TSan的PTY检查均验证延迟页、参考无指标提示、事件单步、快照、80×24下
延迟图与暂停/退出入口可达，以及既有单策略切换和终端恢复。
45秒真人语音constant场景fixed-20的网络/等待/包发送到输出均值分别为
30、64.9896、94.9896ms，三种策略均零PLC。phased场景验证同分母恒等关系：

| 策略 | 播放源网络均值 ms | 等待均值 ms | 包发送到输出均值 ms |
| --- | ---: | ---: | ---: |
| fixed-20 | 33.5576 | 61.4320 | 94.9896 |
| fixed-150 | 38.0311 | 186.9585 | 224.9896 |
| adaptive | 37.7863 | 85.6601 | 123.4464 |

复验命令：`make compare INPUT=assets/real/mandarin.wav SCENARIO=constant`及
同输入`SCENARIO=phased`；数据在各run的CSV中，不包含声卡出声延迟。

网络数值输入补充：基础、jitter、burst背景/附加及reorder附加延迟可通过p表单
的网络页直接编辑，支持F1/F2及键盘页签切换。38项完整测试、26项核心测试通过，
包含35ms非网格基础值、0/1000ms边界、活动场景总延迟上限和固定seed复现。
默认45秒真人语音的network.csv、summary.csv及三种策略WAV与改动前逐字节一致。
本次变更的Config/Network/Options/Experiment共11项测试分别在ASan/UBSan、TSan
构建中通过。普通PTY验证80×24下五个网络字段、页间保留、900ms phased越界错误、
取消还原、新run元数据、constant 100ms实际包到达及延迟页读数；同一组终端检查
也在TSan界面构建下通过。

参数解释补充：策略、scene及全部缓冲/网络字段均有括号含义说明；默认中文、ASCII
英文。标签区44列，字段容器仅纵向滚动，避免裁掉参数名。构建及完整PTY检查通过，
验证80×24中英文说明、数值、应用/取消可见，网络100ms编辑/重跑及退出清理正常。

生成的 WAV、CSV、PTY 转录和构建日志均在忽略目录，不作为源码提交。可复验：

```sh
python3 tools/tui_smoke.py build/<configuration>/cmake/ajb_demo
# 也可显式提供真实语音输入，测试仍保持静音：
python3 tools/tui_smoke.py build/<configuration>/cmake/ajb_demo /path/to/speech.wav
```
