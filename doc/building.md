# 构建与维护文档

文档使用 Sphinx 和 MyST Markdown，入口为 `doc/index.md`，各篇文章由 `toctree`
组织。默认使用 Sphinx 内置主题，不需要 Node.js 或额外主题包。

## 本地构建

在项目根目录执行。Python 工具依赖统一由 uv 管理，默认使用 `.python-version`
指定的 Python 3.13，支持 Python ≥3.11：

```sh
make doc
```

该目标通过 uv 自动同步锁定的 `docs` 依赖组，再执行 Sphinx 严格构建。
浏览器打开 `doc/_build/html/index.html`。`-n` 启用严格引用检查，`-W` 将警告视为失败，
`--keep-going` 一次报告所有页面的问题。也可直接运行以下命令，无需手动激活环境：

```sh
uv run --locked --group docs python -m sphinx -b html -n -W --keep-going doc doc/_build/html
```

`pyproject.toml` 声明构建所需的 Conan，以及 `docs` 组中的 Sphinx/MyST；
`uv.lock` 锁定直接及传递依赖的版本和校验信息，包来源为 PyPI。
`uv sync --locked` 只安装构建工具，增加 `--group docs` 后同时安装文档工具。
环境创建于项目的 `.venv/` 中，Makefile 默认通过 `uv run --locked python` 运行脚本。

调整依赖使用 `uv add`，文档依赖使用 `uv add --group docs`，并一起提交
`pyproject.toml` 和 `uv.lock`。升级时检查上游兼容性与安全公告，再重新执行严格构建。
缓存齐备后可用 `uv sync --locked --group docs --offline` 离线同步。
配置说明见 [uv 项目指南](https://docs.astral.sh/uv/guides/projects/)。

## 编辑约定

- 新文章使用 `.md`，在 `index.md` 的对应目录树中添加条目。
- 文章之间使用相对 Markdown 链接，例如 `[设计](design.md)`，在源码与 HTML 中均可导航。
- 说明算法时对应实际函数和参数，区分参考方案、已实现行为和后续设想。
- 历史验收结果保留环境、输入和日期；未重新执行的测试不标成当前验证。
- HTML 输出与虚拟环境均被 Git 忽略，提交文档源文件即可。
