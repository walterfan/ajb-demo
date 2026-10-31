# Adaptive Jitter Buffer Demo

本项目用可重复的网络轨迹比较固定缓冲与自适应缓冲：先通过试听和数据观察延迟与
欠载，再沿着目标估计、播放决策和 WSOLA 理解实现。它是一个独立的 C++20 项目。

首次运行从[安装与构建](installation.md)开始；理解算法按[设计](design.md)和
[代码实现](implementation.md)阅读；分析实验结果时查阅[输出与指标](metrics.md)。

```{toctree}
:maxdepth: 2
:caption: 使用与实验

installation
usage
metrics
```

```{toctree}
:maxdepth: 2
:caption: 设计与实现

design
implementation
```

```{toctree}
:maxdepth: 2
:caption: 开发与参考

testing
validation
references
building
```
