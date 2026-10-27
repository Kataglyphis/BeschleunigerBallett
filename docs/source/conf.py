# Sphinx configuration: https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
from pathlib import Path

from pygments.lexers.special import TextLexer

# The lock includes the hub's editable DocumANTation theme.
from sphinx_kataglyphis import setup_theme

DOCS_SOURCE_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_SOURCE_DIR.parent.parent


def _find_doxygen_xml_dir() -> Path | None:
    env_override_raw = os.environ.get("KATAGLYPHIS_DOXYGEN_XML_DIR")
    if env_override_raw:
        env_override = Path(env_override_raw)
        if (env_override / "index.xml").exists():
            return env_override

    candidates = [
        REPO_ROOT / "build" / "build" / "xml",
        REPO_ROOT / "build" / "xml",
        REPO_ROOT / "build-clangcl-debug" / "xml",
        REPO_ROOT / "build-clangcl-release" / "xml",
        REPO_ROOT / "build-clangcl-profile" / "xml",
    ]
    for candidate in candidates:
        if (candidate / "index.xml").exists():
            return candidate

    return None


# Project information

project = "BeschleunigerBallett"
release = (REPO_ROOT / "VERSION.txt").read_text(encoding="utf-8").strip()

# Breathe / Exhale (optional C++ API docs)
doxygen_xml_dir = _find_doxygen_xml_dir()
api_extensions = ["breathe", "exhale"] if doxygen_xml_dir is not None else []

# The family theme; css/beschleunigerballett.css holds this site's one departure, the dark hero.
setup_theme(
    globals(),
    repository_url="https://github.com/Kataglyphis/BeschleunigerBallett",
    project_name=project,
    copyright_="2024, Jonas Heinle",
    author="Jonas Heinle",
    extensions_extra=["sphinx.ext.graphviz", "sphinx.ext.inheritance_diagram", *api_extensions],
    html_css_files_extra=["css/beschleunigerballett.css"],
)

# The repository's own logo, in place of setup_theme()'s shared default.
html_logo = "../../images/logo.svg"
html_favicon = html_logo

if doxygen_xml_dir is not None:
    breathe_projects = {"BeschleunigerBallett": str(doxygen_xml_dir)}
    breathe_default_project = "BeschleunigerBallett"
    exhale_args = {
        "containmentFolder": "./api",
        "rootFileName": "library_root.rst",
        "rootFileTitle": "Library API",
        "doxygenStripFromPath": str(REPO_ROOT),
        "createTreeView": True,
        "contentsDirectives": True,
        "exhaleExecutesDoxygen": False,
    }
else:
    # Without Doxygen XML Exhale never writes this page, and the links to it would warn.
    api_stub_dir = DOCS_SOURCE_DIR / "api"
    api_stub_dir.mkdir(exist_ok=True)
    (api_stub_dir / "library_root.rst").write_text(
        ":orphan:\n\n"
        "Library API\n"
        "============\n\n"
        "No Doxygen XML was found for this build, so the generated C++ API "
        "reference is unavailable. Generate Doxygen XML and set "
        "``KATAGLYPHIS_DOXYGEN_XML_DIR`` (or build into one of the paths "
        "listed in ``documentation_workflow.md``), then rebuild the docs to "
        "replace this placeholder with the real reference.\n",
        encoding="utf-8",
    )

# The WebGPU demo, which scripts/linux/docs-build-web.sh builds into this ignored directory.
html_extra_path = ["_webgpu_demo"]

# MyST
myst_enable_extensions = [
    "dollarmath",
    "amsmath",
    "colon_fence",
    "deflist",
]

# Only the html builder auto-excludes html_static_path; linkcheck and latex would read stray .md files there.
exclude_patterns = ["_static/**"]

# Linkcheck gates internal links only; external URLs flake (rate limits, outages).
linkcheck_ignore = [r"^https?://"]

# Graphviz
graphviz_output_format = "svg"

# The build runs with -W; Exhale documents nested types twice (own page and parent's), which no source edit avoids.
suppress_warnings = ["duplicate_declaration.cpp"]


def setup(app):
    # GitHub draws a mermaid fence (model-loading.md); the site shows its source instead of failing -W.
    app.add_lexer("mermaid", TextLexer)
