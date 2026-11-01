# 参考资料

## 设计背景

Walter Fan，《如何实现一个自适应抖动缓冲》，2026-10-29。
用户提供的文章位于个人博客源码仓库：
`wfblog/content/journal/tech_20261029_adaptive-jitter-buffer.md`。
作者网站：[fanyamin.com](https://www.fanyamin.com)。

本文档参考其“到达观测 → 目标估计 → 播放决策 → 时间伸缩 → 度量”的组织思路，
结合本仓库源码重新说明实际实现，未复制原文的图片或示例程序。

| 文章讨论的方案 | 本项目实现 |
| --- | --- |
| RTP 序号、时间戳、网络接收 | 合成包序号与发送/到达时间，无 RTP 解析或回绕处理 |
| 生产者、消费者两个实时线程 | 同一虚拟时钟下的确定性事件循环 |
| 衰减直方图、EWMA、多次峰值检测 | 最近 2 秒精确 P95、即时峰值上调、每 500ms 最多回落 10ms |
| PREFILL / PLAYING / REBUFFER | startup 预缓冲、播放、可选 Rebuffer 与有限 drain；Rebuffer 超时回退 PLC |
| WSOLA、语音 PLC 与 Merge | 有界 WSOLA、静音 PLC 与短淡变 |

## 协议与工程资料

- [WSOLA 原始论文：Roelands、Verhelst，1993](https://www.isca-archive.org/eurospeech_1993/roelands93_eurospeech.html)：基于波形相似性的交叠相加与时间伸缩。
- [NetEQ 的 GetAudio 与操作类型](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_coding/neteq/g3doc/index.md)：时间伸缩、PLC、恢复拼接及舒适噪声的职责区别。
- [RFC 3389](https://www.rfc-editor.org/rfc/rfc3389.html)：舒适噪声参数的 RTP 载荷，与接收端全零 PCM 补帧区分。
- [RFC 3550 §6.4.1](https://www.rfc-editor.org/rfc/rfc3550.html#section-6.4.1)：到达间隔抖动的定义与递推公式。
- [WebRTC NetEQ 源码](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_coding/neteq/)：生产级实现的阅读入口；项目不声明与其算法等价。
- [WebRTC Statistics](https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-jitterbufferdelay)：真实 WebRTC 缓冲累计统计的口径。
- [Sphinx 快速入门](https://www.sphinx-doc.org/en/master/usage/quickstart.html)与 [MyST-Parser](https://myst-parser.readthedocs.io/en/latest/intro.html)：文档结构、Markdown 解析与构建配置。
- [语音样本来源及署名](https://github.com/walterfan/ajb-demo/blob/master/assets/real/ATTRIBUTION.md)：录音来源、截取区间和校验摘要。
