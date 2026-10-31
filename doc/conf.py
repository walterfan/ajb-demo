"""Static Sphinx configuration for the project's MyST documentation."""

project = "Adaptive Jitter Buffer Demo"
author = "Walter Fan"
copyright = "2026, Walter Fan"
extensions = ["myst_parser"]
source_suffix = {".md": "markdown"}
root_doc = "index"
language = "zh_CN"
exclude_patterns = ["_build", ".DS_Store"]
myst_heading_anchors = 3
html_theme = "alabaster"
html_title = project
html_theme_options = {"description": "自适应抖动缓冲：设计、实现与实验"}
html_sidebars = {"**": ["about.html", "navigation.html", "searchfield.html"]}
