# 测试与开发验证

```sh
make test
make asan TUI=OFF
make tsan
# 将下方路径换成 make build 输出的实际可执行路径：
uv run --locked python tools/tui_smoke.py build/<configuration>/cmake/ajb_demo
```

TUI 检查默认使用 `assets/real/mandarin.wav`，也可在可执行文件路径后指定其他 WAV。
算法单元测试使用短合成信号，以验证精确的时序、音高和缺失边界。

Web GUI 的本地 HTTP 与真实渲染检查见 [Web GUI 验证](web-gui.md)。

测试包含常数零欠载、固定大小取舍、同刻截止边界、启动乱序、容量/重复/迟到、
真实缺失时长、尾部/全丢包终止、P95目标升降、音高容差2%、源游标456..504、
回放同刻事件前缀、WAV转换/限制/取消、CSV结果和静音播放暂停重建。PTY检查覆盖
导览、单步、页面、快照、参数重跑、表单字符、缩放、中文/ASCII和q/Ctrl-C终端
恢复；不会播放真实声音。实际环境、验收命令和对比数值见 [验收记录](validation.md)。

若profile不存在，可自行运行 `uv run --locked conan profile detect --name ajb`，再通过
`PROFILE_HOST=ajb PROFILE_BUILD=ajb` 使用；常规构建不会替你执行detect。
有意更新 Conan 锁文件时，在本目录使用 `uv run --locked conan lock create . --lockfile-out=conan.lock`
并显式指定所需profile/设置，审查锁文件差异后验证全功能和核心配置。


## PLC / Rebuffer 对照

欠载可选择 PLC 持续推进或 Rebuffer 冻结源位置重新蓄水；策略参数、超时边界和
独立统计口径见[PLC 与重新缓冲](underrun.md)。
