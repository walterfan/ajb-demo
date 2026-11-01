# PLC 与重新缓冲（Rebuffer）

欠载策略独立于“小固定 / 大固定 / 自适应”缓冲策略。默认选择 PLC；Web GUI 左侧
可切换欠载策略，再运行相同场景和种子，试听并下载两次结果。两次运行的
`network.csv` 相同，便于区分欠载处理方式带来的影响。

## 两种选择

| 欠载策略（Underrun policy） | 缺少当前源音频时 | 代价 |
| --- | --- | --- |
| 丢包补偿（PLC, Packet Loss Concealment） | 用短淡变和静音补齐输出，继续推进源位置 | 保持播放时间线，迟到音频可能被跳过；本项目不重建语音 |
| 重新缓冲（Rebuffer） | 输出等待静音，冻结源位置，继续接收和积累音频 | 能保留等得到的迟到音频，但产生停顿与额外延迟 |

设备输出始终每 10ms 产生 480 个样本。Rebuffer 的“暂停”指停止消费源音频，
并非停止输出时钟；WAV 中包含实际等待的静音，因此可以听到停顿。

对于按块生成、经可靠传输送来的 TTS，偶尔生成不及通常更适合 Rebuffer，以保留
完整语句；实时对话若更在意响应时限，可选择 PLC。Rebuffer 无法恢复真正丢失的
音频，效果取决于音频最终是否到达。

## 状态与边界

- 预缓冲（Prefill）：起播前积累预算与 40ms 前视，不计入 PLC 或 Rebuffer。
- 播放（Playing）：按当前源位置输出；下一块有效源样本不连续时触发欠载策略。
  仅缺少 WSOLA 的额外前视、但当前 10ms 音频仍完整时，不触发 Rebuffer。
- 重新缓冲（Rebuffer）：源位置保持不动，WSOLA 历史重置。连续水位达到
  `max(rebuffer_resume_ms, 当前预算 + 40ms)` 时恢复；末尾按剩余有效源长度截断水位要求。
- 单次等待达到 `rebuffer_timeout_ms` 后回退 PLC，推进当前源区间，避免永久丢包阻塞。
  本次实验每条输出轨的累计等待上限为 5000ms，耗尽后其余欠载直接使用 PLC。

默认恢复水位下限为 100ms，单次等待上限为 300ms；这两个值仅影响 Rebuffer。
恢复水位为 **包含前视的总连续水位**。它与左侧固定缓冲的“额外预算”含义不同。
恢复前后使用最多 5ms 淡变；等待期间仍执行去重、迟到判断、容量保护与目标估计。
队列容量仍为 500ms，长时间等待可能造成溢出。

## 命令行对照实验

```sh
make compare ARGS='--scenario phased --seed 42 --underrun-policy plc'
make compare ARGS='--scenario phased --seed 42 --underrun-policy rebuffer --rebuffer-resume-ms 100 --rebuffer-timeout-ms 300'
```

默认使用 `assets/real/mandarin.wav`，每次创建新的 `out/run-N/`。TUI 也接受相同 CLI
参数，参数表单重跑时保留这些设置。Web GUI 每次产生三个缓冲策略的输出及原始参考轨；
切换欠载策略后需重新运行，下载结果可保留前一次实验。

## 如何读结果

同时比较平均缓冲等待（Mean sample wait）、PLC 时长、Rebuffer 时长、超时次数、
未播放源比例与实际听感。不能仅以 PLC 减少判定更好：Rebuffer 可能用更长停顿换取它。
平均缓冲等待只针对实际播放的有效源样本，等待期间源位置不动，后续样本的等待可能增长。

`summary.csv` 新增 `rebuffer_ms`、`rebuffer_events`、`rebuffer_timeouts`；
每帧 CSV 新增累计 `rebuffer_total_ms`、`rebuffer_events`、`rebuffer_timeouts`。
等待块 `action=rebuffer`、`consumed_samples=0`、`concealed_samples=0`，无有效样本延迟
统计；不会增加 PLC 或媒体播放块计数。事件日志记录 `rebuffer_start` 和 `rebuffer_end`，
结束原因区分 `level_recovered` 与 `timeout_fallback_plc`。`experiment.txt` 保存策略及参数。

实现不新增依赖。CLI 与 HTTP 参数均限制策略枚举、整数范围和 10ms 步长；
HTTP 仍限本机、同源与单任务，音频和输出规模沿用既有上限。
