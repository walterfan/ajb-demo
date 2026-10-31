# Adaptive jitter-buffer demo

通过同一网络轨迹下的试听、队列和事件回放，比较小固定、大固定与自适应抖动
缓冲的连续性和延迟。提供中文 TUI、离线 WAV/CSV 导出及完整构建和安装说明。

## 构建与依赖

演示独立于根目录的 `aca`，要求 C++20、CMake ≥3.20、Conan ≥2.28 且 <3。
Conan 固定 `ftxui/7.0.3`（MIT）和 `gtest/1.18.0`（BSD-3-Clause），来源为
ConanCenter 配方或用户配置的镜像；真实生成的 `conan.lock` 锁定版本和 recipe
revision。GStreamer 使用下文的完整系统 SDK，不混用 Conan GStreamer core。

```sh
cd demo/ajb
make deps                       # 按锁文件准备依赖
make doctor                     # 只读环境检查
make build
make start                      # 交互终端中的导览
make compare INPUT=/path/to/speech.wav  # 不打开声卡或全屏终端
make test
make test MEDIA=OFF TUI=OFF      # 只构建核心测试，不要求 GStreamer/FTXUI
make test MEDIA=OFF TUI=OFF OFFLINE=ON  # 已缓存依赖的离线验证
```

`PROFILE_HOST`/`PROFILE_BUILD` 默认使用现有 `default`；缺失时先自行准备 profile，
构建不会覆盖用户配置。可用 `BUILD_TYPE=Debug`、`JOBS=8`、`TUI=OFF`、`TESTS=OFF`
选择配置。不同配置保存在 `build/` 的独立子目录，CMake 显式使用 Conan toolchain，
不依赖 generated presets。`make asan` 与 `make tsan` 使用独立 Debug 目录；
第三方依赖默认是未插桩的 Conan 库，报告中区分应用和第三方插桩范围。
`make clean` 只清除演示的 build/out/assets，不动 Conan 全局缓存或锁文件。

Conan 锁文件不锁定编译器和系统媒体 SDK。当前验证环境为 macOS arm64、
AppleClang 21、Conan 2.28.1、GStreamer 1.28.8；用 `make doctor` 查看实际环境。

## 实验机制

源 WAV 显式归一化为 48kHz、单声道、S16LE，每帧 480 样本（10ms）。末尾保留
有效样本长度，不足一帧补零，补零不算丢包。输入上限 128MiB/300秒。
默认样本是固定 seed 的30秒类语音，不是真人录音；可用真实语音 WAV 比较听感。

同一实验的三种策略共用一条网络到达轨迹。核心按虚拟时钟处理到达事件，
同刻先到达再播放，缓冲看不到未来到达。试听播放渲染出的同一结果，终端刷新
不影响算法。缓存按源序号排序，保留缺洞，过期不重播；总容量500ms，溢出丢最远
未来帧。缺帧输出静音 PLC，进入/恢复做不超过5ms的淡变；它不是语音重建算法。

固定策略默认预算20/150ms。自适应默认 min/initial/max=20/60/200ms，使用最近2秒
到达延迟的P95加10ms余量，并对新峰值快速增大目标，稳定后每500ms最多减10ms。
有效迟到包先更新估计再被媒体缓存丢弃，重复包不重复采样。RFC3550 J 是观察量，
不直接等于目标。预算必须为0..460ms的10ms整数倍，min ≤ initial ≤ max。

**预算、目标水位与实际水位不同**：所有策略共同预留40ms算法前视，所以预算20ms
对应目标水位60ms。实际水位只计算当前游标之后连续可播放的数据，缺洞后面的包
只计入缓存跨度。自适应用 WSOLA 保持音高，每tick输出480样本，实际源游标推进
456..504样本：水位低则少消费（SLOW），高则多消费（FAST）。修正100ms偏差约需
2秒；未知突发仍可能 PLC。发送结束后停止主动扩展并有限排空。

| SCENARIO | 默认网络条件与观察目标 |
| --- | --- |
| constant | 恒定30ms，启动后零网络欠载 |
| jitter | 30ms + 均匀0..80ms（10ms步长），比较小/大缓冲 |
| burst | 30ms + 0..20ms；1秒后每500ms的前100ms再加120ms |
| reorder | 每12帧的第一帧额外延迟70ms，观察缺洞被补齐 |
| loss | 30ms延迟，固定seed下约5%独立丢失；缓冲不能恢复真丢包 |
| phased | 0–5秒稳定；5–10秒jitter；10–15秒burst；15秒后稳定恢复 |

通过 `SCENARIO=constant SEED=42` 或对应CLI选项控制。随机采样保证同版本同seed
一致，不承诺不同标准库之间逐位一致。网络单包延迟上限1000ms。

### 直接填写网络延迟

按 **`p`** 打开参数表单，按 **F2** 进入“网络”页；也可 Shift-Tab 到页签后用
左右方向键切换。“缓冲/策略”页可按F1返回，两页的待应用输入会一起保留。
每个参数名后都有括号解释，例如`base delay ms (基础单程网络延迟)`、
`min ms (自适应预算下限)`；`--ascii`模式使用英文解释。

| 网络字段 | 默认值 | 使用场景 |
| --- | ---: | --- |
| `base delay ms` | 30 | 所有场景的基础单程网络延迟 |
| `jitter max ms` | 80 | jitter随机附加0..该值；phased的5–10秒阶段 |
| `burst jitter ms` | 20 | burst背景随机抖动；phased的10–15秒阶段 |
| `burst extra ms` | 120 | 突发窗口额外增加的延迟 |
| `reorder extra ms` | 70 | reorder中每12帧第一帧额外延迟 |

基础值和附加延迟支持 **0..1000ms整数**，包括35ms这样的值；两种随机抖动幅度
要求 **10ms整数倍**。当前场景的 `基础值 + 最大有效附加值` 必须≤1000ms。
constant/loss仅使用基础值，其他页中的非活动附加项不参与总延迟。

例如设置 **`scene=constant`、`base delay ms=100`**，Tab到“应用并重新运行”并
按Enter，新实验的包传输延迟即为100ms；按`d`可看N曲线/数值。若选jitter并设
基础值50、jitter max为80，则传输延迟在50–130ms内变化。
错误会在表单内提示；Esc取消并保留原实验，成功应用后创建新run。seed会保留。
所有网络参数记录在`experiment.txt`，CLI也可复现：

```sh
make start INPUT=assets/real/mandarin.wav SCENARIO=constant ARGS='--network-delay-ms 100'
make compare SCENARIO=jitter ARGS='--network-delay-ms 50 --jitter-ms 80'
# 其他选项：--burst-jitter-ms 20 --burst-delay-ms 120 --reorder-delay-ms 70
```

## TUI 导览

### 用连贯真人语音试听

提供 Jing Li 朗读鲁迅《朝花夕拾·小引》的连续45秒中文片段，取原录音30–75秒，
保留自然停顿，覆盖默认phased的全部阶段。来源为
[LibriVox录音](https://librivox.org/chao-hua-si-she-by-lu-xun/)；
[Internet Archive存档](https://archive.org/details/chao_hua_si_she_jl_librivox)
标注 Public Domain Mark 1.0，LibriVox声明录音在美国属于公有领域，详见其
[许可说明](https://librivox.org/pages/public-domain/)。

```sh
cd demo/ajb
# 获取样本另需 curl 和 ffmpeg（macOS: brew install ffmpeg）
make fetch-speech
make start INPUT=assets/real/mandarin.wav
make compare INPUT=assets/real/mandarin.wav SCENARIO=phased
```

`make fetch-speech` 校验固定源文件的SHA-256，限制下载8MiB及超时，仅截取单个
连续区间并转换为48kHz/mono/S16LE；已有校验通过的MP3会复用，后续无需联网。
来源、朗读者、许可、截取区间和摘要写入 `assets/real/ATTRIBUTION.md`。
音频及原MP3均在忽略目录，`make clean`会清除，可重新获取。进入TUI后按`p`
选择单策略或三策略对比。默认30秒合成样本继续用于可重复的算法回归。

### 操作与机制

首次 `make start` 进入就绪页，Enter 准备结果并依次试听参考、小固定、大固定和
自适应。准备可以取消；三策略在同一实验时间轴上并列展示，当前只播放一个轨道。
参考绕过网络/缓冲，没有伪造的队列指标。同一实验时刻可能对应不同源位置，
并不表示正在听同一个音节。最小终端80×24，推荐120×36。

要单独测试某种策略，按 **`p`**（或点击“策略/参数”），在“测试策略”中选择
**小固定缓冲、大固定缓冲、自适应（单独）或自定义固定缓冲**，再按 Tab 移到
“应用并重新运行”并按 Enter。策略下拉框先按 Enter 展开，用方向键选择，再按
Enter 确认。小/大固定使用 `small ms`/`large ms`，自定义固定
使用 `custom ms`；预算为0..460ms的10ms整数倍。单独测试只生成所选策略的结果，
直接播放该策略，结束后停在结果页。切回“三策略对比”可恢复默认四轨导览。
关闭表单或取消不会应用待选策略；同一seed保留以便比较。CLI 的
`--mode fixed --buffer-ms 80` 或 `--mode adaptive` 也会在 TUI 中保留。

- **总览**：预算、目标水位、连续水位、累计补偿、源位置与当前动作；宽屏提供
  目标/实际水位曲线。紧凑终端保持三行比较表。
- **机制**：包序号、缺洞、按序缓存、当前源游标、已发生决策的真实消费量、事件
  列表与原因。未来到达隐藏，下一tick的源需求仅是暂定预览。迟到包的估计更新
  和媒体丢弃分别记录，不用 `序号×帧长+target` 冒充实际截止时间。
- **结果**：整轮汇总及输出路径，和截至当前游标的统计分开；进入结果页会暂停。
- **延迟**：按`d`打开最近5秒的延迟曲线，横轴为实验时间、纵轴为毫秒，三条
  曲线共用自动刻度（50ms步长）；`+`表示曲线点重叠。

| 延迟页标记 | 定义 |
| --- | --- |
| N：到达 | 最近观察到的包 `arrival - send`，包含基础网络延迟及抖动 |
| B：等待 | 最近10ms输出块中有效源样本的 `output - arrival` 均值 |
| P：发送至输出 | 同一批有效源样本的 `output - packet_send` 均值 |

当前到达包和当前播放源可能不同。页面另列当前播放源的网络延迟，满足
`P = played-N + B`；显示的J是RFC到达抖动估计。B/P按首次贡献的真实源样本计权，
WSOLA重叠不重复计数。启动、全PLC或没有新有效样本的输出块显示`--`及曲线空缺，
不伪造零延迟。数值包含共同40ms前视，声卡实际出声延迟未测量。

查看示例：选择`phased`并运行，按`3`试听自适应，按`d`看延迟；按Space暂停，
`n`逐事件、`.`逐10ms，时间线获得焦点后用左右键定位。5–10秒观察到达延迟起伏，
10–15秒观察突发和补偿，15秒后观察恢复。机制页可查包的send/arrival/transit。

| 操作 | 按键 |
| --- | --- |
| 开始/暂停/继续 | Enter（就绪或按钮）；Space（组件优先处理） |
| 选择页面 | 页签方向键；`v`循环总览/机制/结果/延迟；`d`直接看延迟 |
| 焦点前后移动 | Tab / Shift-Tab |
| 下一10ms播放tick / 下一事件 | `.` / `n`，保持暂停和静音 |
| 参考 / 小固定或自定义固定 / 大固定 / 自适应 | `0` / `1` / `2` / `3`，从零重新试听；未运行的策略提示按 `p` 选择 |
| 重播当前轨道 | `r` |
| 定位 ±1秒 | 时间线获得焦点后左右方向键 |
| 可视速度0.25×/0.5×/1×/2× | `-` / `+`；非1×明确静音 |
| 策略、场景和参数表单 | `s` / `p` |
| 保存当前机制的纯文本快照 | `e` |
| 帮助 / 关闭弹层 / 退出 | `?` / Esc / `q`；Ctrl-C也会清理终端 |

参数表单标明“待应用”，输入字符不会触发全局快捷键。应用时验证范围，使用
保留的seed生成新run；失败/取消保留上一轮完整结果。新结果准备完成前，原配置
与原音频保持对应。可选择关键事件暂停讲解；教学暂停不增加实验PLC计数。
表单有“缓冲/策略”和“网络”页签，F1/F2快速切换，Tab遍历当前页的字段与按钮；
输入较多或展开下拉框时，字段区域会随焦点滚动，应用/取消按钮保持可访问。

学习建议：先选constant观察预缓冲，再选burst，试听`1`与`2`，暂停后用`n`查看
缺帧/迟到的事件顺序；最后选phased，在自适应轨道观察目标升高、SLOW积累水位、
恢复阶段FAST降低延迟。使用真实语音时通过 `INPUT=/absolute/path/speech.wav`。

正常有声播放使用GStreamer媒体位置驱动画面。暂停后定位/单步会清空旧播放队列，
恢复1×从新游标重新对齐。无声卡可继续“仅可视回放”，设备错误不计入网络欠载。
终端过小时自动暂停，恢复尺寸后手动继续；支持 `--ascii`、`--no-color` 和
`NO_COLOR`。非TTY自动模式退回无声卡的离线比较，不输出全屏控制序列。

```sh
make start ARGS='--ascii --no-color'
make start ARGS='--no-tui --play'       # 显式纯文本试听
make compare ARGS='--mode fixed --buffer-ms 80'
make compare ARGS='--mode adaptive'
make compare INPUT='/absolute/path/with spaces.wav' SCENARIO=burst
```

## 输出与指标

每轮在 `OUT`（默认out）下创建新的 `run-N/`，不覆盖旧实验。失败的未完成run
会清理。音频和事件都留在本地，不上传；诊断不打印PCM或WAV元数据。

| 文件 | 内容 |
| --- | --- |
| `fixed-<ms>.wav` / `adaptive.wav` | 含启动静音、PLC及正常排空的试听音频 |
| 同名 `.csv` | 逐tick输出时间(ns)、源样本、消费量、预算/水位(ms)、动作、补偿样本与累计计数 |
| `network.csv` | 同版本同seed可复现的发送/到达ns及网络丢失标记 |
| `events.csv` | 策略、稳定事件序号、ns、类型、原因、包、源位置及实际动作；可检查同刻事件前缀 |
| `summary.csv` | 启动、平均/P95等待与发送到输出延迟、目标/水位分布、PLC、丢弃原因、伸缩比例 |
| `experiment.txt` | run、模式、固定预算、场景、seed、格式、帧长及预算配置 |
| `snapshot-*.txt` | 当前run/策略/时间/事件序号的纯文本机制快照 |

时间统一为实验时钟；设备真实声学延迟未测量。缓冲等待为有效源样本首次实际
输出减入队时刻，发送到输出为首次输出减源发送时间；WSOLA重叠的同一源样本
只计一次。缺失样本没有真实入队时间，不进入等待分布；未播放源区间比例单列，
防止丢弃慢包让平均值显得过好。空的真实播放分布输出nan。

PLC次数按连续缺失区间计，时长为补偿样本/48；另列PLC tick数。FAST/SLOW比例
分母为有效媒体tick。启动静音、共同40ms前视、末尾补零及正常排空不算欠载，
主动拉伸不算PLC。不输出未经计算的MOS，也不保证自适应在所有网络中都更好。

事件上限每策略120万，外部到达轨迹最多10万条；回放使用每秒检查点与事件前缀，
不为每次接收复制完整缓存。超限明确失败，不静默丢事件。

逐tick CSV另有 `delay_samples`（该块首次贡献的有效源样本数）、
`played_network_ms`、`buffer_wait_ms`、`packet_to_output_ms`，后三者为该块均值，
无有效贡献时为nan；`wait_mean_ms`仍为截至该tick的累计均值。
`events.csv`的`network_delay_ms`只在到达事件有值。汇总新增
`played_network_mean_ms`和`packet_to_output_mean_ms`，按相同源样本分母计权。
原有`send_to_output_*`按源样本时间戳计算音频年龄；`packet_to_output_*`按整包
发送时刻计算，区分包内样本相位，不能把这两种口径混用。

## 开发验证

```sh
make test
make asan TUI=OFF
make tsan
# 将下方路径换成 make build 输出的实际可执行路径：
python3 tools/tui_smoke.py build/<configuration>/cmake/ajb_demo
```

测试包含常数零欠载、固定大小取舍、同刻截止边界、启动乱序、容量/重复/迟到、
真实缺失时长、尾部/全丢包终止、P95目标升降、音高容差2%、源游标456..504、
回放同刻事件前缀、WAV转换/限制/取消、CSV结果和静音播放暂停重建。PTY检查覆盖
导览、单步、页面、快照、参数重跑、表单字符、缩放、中文/ASCII和q/Ctrl-C终端
恢复；不会播放真实声音。实际环境、验收命令和对比数值见 [VALIDATION.md](VALIDATION.md)。

若profile不存在，可自行运行 `conan profile detect --name ajb`，再通过
`PROFILE_HOST=ajb PROFILE_BUILD=ajb` 使用；常规构建不会替你执行detect。
有意更新锁文件时，在本目录使用 `conan lock create . --lockfile-out=conan.lock`
并显式指定所需profile/设置，审查锁文件差异后验证全功能和核心配置。

## Install GStreamer on macOS

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
