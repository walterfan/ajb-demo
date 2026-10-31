# 实验与 TUI 操作

默认输入为 `assets/real/mandarin.wav`，是一段 45 秒的真人中文语音。
`make start`、`make compare` 和 TUI 自动检查均使用该样本；指定 `INPUT=...`
可替换 Make 命令的输入，直接运行程序时可用 `--input` 覆盖默认路径。

## 网络场景

| SCENARIO | 默认网络条件与观察目标 |
| --- | --- |
| constant | 恒定100ms，启动后零网络欠载 |
| jitter | 100ms + 均匀0..120ms（10ms步长），比较小/大缓冲 |
| burst | 100ms + 0..30ms；1秒后每500ms的前100ms再加160ms |
| reorder | 每12帧的第一帧额外延迟100ms，观察缺洞被补齐 |
| loss | 100ms延迟，固定seed下约5%独立丢失；缓冲不能恢复真丢包 |
| phased | 0–5秒稳定；5–10秒jitter；10–15秒burst；15秒后稳定恢复 |

通过 `SCENARIO=constant SEED=42` 或对应CLI选项控制。随机采样保证同版本同seed
一致，不承诺不同标准库之间逐位一致。网络单包延迟上限1000ms。

默认 `phased` 在前 5 秒保持平稳，5–10 秒到达延迟为 100–220ms，10–15 秒的
突发窗口为 260–290ms，15 秒后恢复至 100ms。按 `1`、`2`、`3` 从头试听三种策略，
比较小固定的断续、大固定的等待和自适应的恢复。单纯增大恒定延迟主要增加起播
等待；音频中的断续主要来自变化的抖动和突发。

## 配置网络延迟

按 **`p`** 打开参数表单，按 **F2** 进入“网络”页；也可 Shift-Tab 到页签后用
左右方向键切换。“缓冲/策略”页可按F1返回，两页的待应用输入会一起保留。
每个参数名后都有括号解释，例如`base delay ms (基础单程网络延迟)`、
`min ms (自适应预算下限)`；`--ascii`模式使用英文解释。

| 网络字段 | 默认值 | 使用场景 |
| --- | ---: | --- |
| `base delay ms` | 100 | 所有场景的基础单程网络延迟 |
| `jitter max ms` | 120 | jitter随机附加0..该值；phased的5–10秒阶段 |
| `burst jitter ms` | 30 | burst背景随机抖动；phased的10–15秒阶段 |
| `burst extra ms` | 160 | 突发窗口额外增加的延迟 |
| `reorder extra ms` | 100 | reorder中每12帧第一帧额外延迟 |

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
cd ajb-demo
# 获取样本另需 curl 和 ffmpeg（macOS: brew install ffmpeg）
make fetch-speech
make start
make compare SCENARIO=phased
```

`make fetch-speech` 校验固定源文件的SHA-256，限制下载8MiB及超时，仅截取单个
连续区间并转换为48kHz/mono/S16LE；已有校验通过的MP3会复用，后续无需联网。
来源、朗读者、许可、截取区间和摘要写入 `assets/real/ATTRIBUTION.md`。
仓库包含音频样本及原 MP3；`make clean` 会删除 `assets/`，可用上述命令重新获取。进入TUI后按`p`
选择单策略或三策略对比。算法单元测试继续使用可控的合成信号验证特定边界。
需要手动对比 30 秒合成样本时，可执行：

```sh
make fixtures
make compare INPUT=assets/speech.wav
```

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
