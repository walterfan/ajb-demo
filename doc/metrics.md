# 输出文件与指标

## 实验产物

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

## 延迟的计算口径

时间统一为实验时钟；设备真实声学延迟未测量。缓冲等待为有效源样本首次实际
输出减入队时刻，发送到输出为首次输出减源发送时间；WSOLA重叠的同一源样本
只计一次。缺失样本没有真实入队时间，不进入等待分布；未播放源区间比例单列，
防止丢弃慢包让平均值显得过好。空的真实播放分布输出nan。

令某个源样本首次贡献输出的时刻为 `output`，所属包的发送、到达时刻为 `send`、
`arrival`。在相同源样本集合和权重下：

```text
played_network = arrival - send
buffer_wait = output - arrival
packet_to_output = output - send = played_network + buffer_wait
```

TUI 的 N 曲线显示最近到达包的网络延迟；B/P 显示当前输出块有效贡献的均值。
最近到达的包和当前播放源可能不同，因此不能直接将图上的 N 与 B 相加当作 P。
使用同一批播放源的 `played_network_ms` 才满足上式。

逐 tick CSV 的 `delay_samples` 是该块首次贡献的有效源样本数；
`played_network_ms`、`buffer_wait_ms`、`packet_to_output_ms` 是这些贡献的均值。
无有效贡献时输出 nan；`wait_mean_ms` 为截至该 tick 的累计均值。
`events.csv` 的 `network_delay_ms` 只在到达事件有值。

汇总的 `played_network_mean_ms` 和 `packet_to_output_mean_ms` 使用相同源样本分母。
`send_to_output_*` 按源样本时间戳计算音频年龄，`packet_to_output_*` 按整包发送
时刻计算，两者相差包内样本相位，不能混用。

## 连续性与资源指标

PLC次数按连续缺失区间计，时长为补偿样本/48；另列PLC tick数。FAST/SLOW比例
分母为有效媒体tick。启动静音、共同40ms前视、末尾补零及正常排空不算欠载，
主动拉伸不算PLC。不输出未经计算的MOS，也不保证自适应在所有网络中都更好。

事件上限每策略120万，外部到达轨迹最多10万条；回放使用每秒检查点与事件前缀，
不为每次接收复制完整缓存。超限明确失败，不静默丢事件。

## 如何比较策略

使用相同输入、场景和 seed，对照 `summary.csv` 中的平均/P95 等待、PLC 时长、
迟到包数与未播放源比例。先用 `constant` 检查基线，再用 `phased` 观察恶化和恢复；
同时查看逐 tick 的目标、连续水位和 FAST/SLOW 动作，判断目标变化是否转化为水位变化。

既有合成语音和真实语音的对比数据见[验收记录](validation.md)。这些数据是特定输入
与轨迹的结果，不能推广为自适应在所有网络下都优于固定策略。
