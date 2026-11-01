# 代码实现

## 模块与入口

以下路径均相对于项目根目录。保留现有 `aca::ajb` 命名空间和 `include/aca/ajb/`
头文件路径不影响独立构建；构建入口是本项目自己的 `CMakeLists.txt` 和 `Makefile`。

| 文件 | 关键类型 / 函数 | 责任 |
| --- | --- | --- |
| `include/aca/ajb/core.h`、`src/core.cpp` | `Config`、`Packet`、`DelayEstimator`、`Wsola`、`simulate` | 参数、网络轨迹、缓冲和离线渲染 |
| `include/aca/ajb/replay.h`、`src/replay.cpp` | `Cursor`、`View`、`Replay`、`view_at`、`delays_at` | 按事件前缀还原状态及延迟曲线 |
| `include/aca/ajb/media.h`、`src/media.cpp` | `read_wav`、`write_wav`、`Playback` | GStreamer 格式转换、导出和设备播放 |
| `include/aca/ajb/app.h`、`src/app.cpp` | `Options`、`Experiment`、`parse_options`、`prepare` | 参数校验、实验编排、CSV 与快照 |
| `src/main.cpp`、`src/tui.cpp` | 程序入口、`run_tui` | 命令行模式选择及 FTXUI 交互 |
| `tools/build.py`、`conanfile.py` | 构建驱动、Conan 配方 | 按配置隔离构建，固定第三方依赖 |
| `tools/web_server.py`、`web/` | `ExperimentConfig`、`create_app`、浏览器页面 | 校验参数，调用 C++ 渲染，提供音频与可视化 |

推荐阅读顺序：`Config/Packet` → `make_network()` → `DelayEstimator` →
`simulate()` → `Wsola::process()` → `prepare()` → `view_at()`。
可在[源码目录](https://github.com/walterfan/ajb-demo/tree/master/src)对照阅读。

## 实验编排

`parse_options()` 严格解析整数并校验模式、预算和网络参数。`prepare()` 将 WAV
归一化一次、生成网络轨迹一次，再根据模式调用 `simulate()`：

- `compare`：小固定、大固定、自适应各一次。
- `fixed`：仅生成指定预算的一种固定策略。
- `adaptive`：仅生成自适应结果。

`Experiment` 保存配置、参考音频、轨迹和所有策略结果，导出到新建的 `run-N/`。
渲染失败或取消会清理未完成的目录。TUI 修改参数时先准备新结果，成功后才替换
当前实验，避免界面配置与正在播放的音频不一致。

## `simulate()` 的状态和执行顺序

算法状态包括有序 `std::map<size_t, Packet>` 缓存、去重集合、源样本游标、
估计器、WSOLA 历史、已贡献源样本标记，以及 tick/event 记录。
源 PCM 是不可变数组；缓存保存包的可用性与时间信息，不为每次入队复制 PCM。

```text
校验输入和轨迹 → 排序发送/到达事件 → 确定起播时刻
  ↓
每个 10ms tick：
  记录发送/网络丢失
  处理已到达包：到达 → 去重 → 估计 → 迟到/容量判断 → 按序入队
  起播前：追加静音，记录 startup，进入下一 tick
  媒体阶段：更新目标 → 计算连续水位/跨度
           → 可选 Rebuffer：冻结源位置并输出等待静音；恢复或超时后继续
           → 选择 456/480/504
           → WSOLA 或直接输出/PLC → 记录贡献与指标
           → output 事件 → commit 游标 → 移除已消费包
  ↓
处理网络尾部 → 按时间稳定排序事件 → 重新编号 → 汇总 → 建回放检查点
```

迟到判断是 `(seq + 1) × 480 <= source`：只有整个包区间已消费才判为过期，
包内仍有未来样本时可以接纳。容量既检查前方跨度，也检查实际缓存包数。
事件记录 `kind` 与 `reason`，例如 `duplicate/already_received`、
`late/source_interval_already_committed` 和 `overflow/drop_farthest_future`。

## WSOLA 的波形与来源追踪

原理、示意图及动作选择见 [WSOLA、PLC 与静音帧裁剪](audio-processing.md)。

`Wsola::process()` 使用 20ms 分析片段、10ms 重叠区和合成步长，要求至少 30ms 连续前视；
调用侧仅在具有 40ms 连续数据时进入该路径。搜索在 0–480 样本范围内以 4 样本
为步长，对上一块保留的尾部和候选窗口做归一化相关性比较，再线性交叠相加。

`Block` 除 480 个输出样本外，还记录主、次两路来源偏移。WSOLA 重叠可能让同一
源样本多次贡献输出，`played` 标记保证延迟分布只在它首次贡献时计数。历史尾部
仍保留来源，即使对应包已经从缓存移除，也能识别真实贡献。输出样本饱和到 int16。

游标只按决策中的 `advance` 推进，相关性搜索不提交游标。缺帧时重置重叠历史，
缺失区间用静音和短淡变衔接。音高容差、推进边界和缺失时长有对应核心测试。

### PLC 与静音裁剪的实现边界

`simulate()` 的 `sample()` 根据包是否已在缓存中判断源样本可用性。无 40ms 连续
前视时重置 WSOLA，并直接读取当前块；缺失样本使 `concealed` 增加，再通过
`fade_out`、`fade_in` 和 `previous_output` 做最长 240 样本的静音衔接。
正常到达的零值样本仍是可用数据，不会被误记为 PLC。

当前没有能量检测或主动静音裁剪分支。若添加，需要把跳过的源区间、独立裁剪计数
和事件加入游标提交及回放链路，同时处理 WSOLA 保留尾部和延迟来源追踪；仅删除
`queue` 元素会制造缺洞，随后仍会补静音。该扩展应独立验证静音误判、裁剪限额、
有声内容保护、时间连续性与统计分类。

## 事件回放与试听同步

事件最终按时间稳定排序，并用 `order` 区分同一时刻的多个动作。`Cursor` 是
`(time, order)`；`view_at()` 仅应用截至该游标的事件前缀：

1. 从不晚于前缀的检查点恢复缓存和接收计数，再增量应用后续事件。
2. 仅在 `output` 事件可见后展示对应 tick；源游标等待 `commit` 事件后推进。
3. 目标与抖动取前缀中最后可见的 `target`、`estimate`，避免显示未来结果。

检查点约每秒保存一次队列状态，避免为每次到达复制完整缓存。`delays_at()` 在
同一事件前缀内提取最近 5 秒的网络到达和已经输出的块均值。

有声 1× 播放使用 GStreamer 媒体位置驱动画面；暂停、定位和单步后重建播放队列，
从新游标恢复。非 1×、单步和无设备时保持静音。播放设备错误不会反向修改离线
结果或计入网络欠载。

## 输入、资源与构建边界

| 边界 | 现有约束 |
| --- | --- |
| 音频输入 | 本地 WAV，最大 128MiB、300 秒，转换为 48kHz/mono/S16LE |
| 网络轨迹 | 最多 10 万条；检查序号、发送时间、arrival ≥ send 及 0–1000ms transit |
| 缓存与事件 | 缓存容量 500ms，每策略最多 120 万事件，超限明确失败 |
| 文件路径 | 拒绝 `..` 和路径中的符号链接，检查输入与输出冲突 |
| 媒体对象 | 通过 RAII 包装 GStreamer 对象、缓冲映射及播放生命周期 |
| 输出 | 音频和日志留在本地，不上传 PCM；新 run 避免覆盖已有实验 |

文档配置只启用 MyST，不执行 Markdown 中的代码块。C++ 程序构建不依赖文档环境。
FTXUI、GoogleTest 使用 Conan 锁文件；GStreamer 使用同一系统 SDK 的 core/app/audio。
验证命令和第三方库插桩范围见[测试](testing.md)。


## PLC / Rebuffer 对照

欠载可选择 PLC 持续推进或 Rebuffer 冻结源位置重新蓄水；策略参数、超时边界和
独立统计口径见[PLC 与重新缓冲](underrun.md)。
