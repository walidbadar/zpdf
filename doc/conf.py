# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

# Sphinx configuration of the zpdf documentation.
#
#   pip install -r doc/requirements.txt
#   sphinx-build -b html doc doc/_build/html

project = "zpdf"
author = "Muhammad Waleed Badar"
copyright = "2026, Muhammad Waleed Badar"

extensions = [
    "sphinx.ext.githubpages",
]

exclude_patterns = ["_build", "requirements.txt"]

primary_domain = "c"
highlight_language = "none"

html_theme = "furo"
html_title = "zpdf"
html_logo = "images/zpdf-logo.svg"
html_favicon = "images/zpdf-logo.svg"
html_static_path = []
html_theme_options = {
    "source_repository": "https://github.com/walidbadar/zpdf",
    "source_branch": "main",
    "source_directory": "doc/",
}
