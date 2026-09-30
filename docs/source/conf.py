# Sphinx configuration: https://www.sphinx-doc.org/en/master/usage/configuration.html

from pathlib import Path

# Project rules load after the generated brand CSS (css/custom.css, never fork it); every branch must append this.
PROJECT_CSS_FILE = "css/beschleunigerballett.css"

# A wrong path here is silent: .exists() is False and the fallback below replaces the shared baseline.
ANTFRASTRUCTURE_CONF = (
    Path(__file__).parent.parent.parent
    / "third_party/ANTfrastructure/third_party/DocumANTation"
    / "docs-tooling/source_templates/sphinx-book/conf_base.py"
)
if ANTFRASTRUCTURE_CONF.exists():
    import importlib.util

    spec = importlib.util.spec_from_file_location("conf_base", str(ANTFRASTRUCTURE_CONF))
    if spec and spec.loader:
        conf_base = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(conf_base)

        extensions = conf_base.SPHINX_EXTENSIONS.copy()
        html_theme = conf_base.HTML_THEME
        html_theme_options = conf_base.HTML_THEME_OPTIONS.copy()
        html_static_path = conf_base.HTML_STATIC_PATH
        html_css_files = [*conf_base.HTML_CSS_FILES, PROJECT_CSS_FILE]
    else:
        extensions = ["myst_parser", "sphinx_design"]
        html_theme = "sphinx_book_theme"
        html_theme_options = {
            "repository_url": "https://github.com/Kataglyphis/BeschleunigerBallett",
            "use_repository_button": True,
            "show_navbar_depth": 2,
            "navigation_with_keys": True,
        }
        html_static_path = ["_static"]
        html_css_files = ["css/custom.css", PROJECT_CSS_FILE]
else:
    extensions = ["myst_parser", "sphinx_design"]
    html_theme = "sphinx_book_theme"
    html_theme_options = {
        "repository_url": "https://github.com/Kataglyphis/BeschleunigerBallett",
        "use_repository_button": True,
        "show_navbar_depth": 2,
        "navigation_with_keys": True,
    }
    html_static_path = ["_static"]
    html_css_files = ["css/custom.css", PROJECT_CSS_FILE]

DOCS_SOURCE_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_SOURCE_DIR.parent.parent


def _find_doxygen_xml_dir() -> Path | None:
    env_override_raw = __import__("os").environ.get("KATAGLYPHIS_DOXYGEN_XML_DIR")
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
copyright = "2024, Jonas Heinle"
author = "Jonas Heinle"
release = (REPO_ROOT / "VERSION.txt").read_text(encoding="utf-8").strip()

# Project-specific overrides
html_theme_options["repository_url"] = (
    "https://github.com/Kataglyphis/BeschleunigerBallett"
)

# Project-specific extensions
extensions.extend(
    [
        "sphinx.ext.graphviz",
        "sphinx.ext.inheritance_diagram",
    ]
)

# The WebGPU demo; rebuild per third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-gltf-rust-plan.md
html_extra_path = ["_webgpu_demo"]

# MyST
myst_enable_extensions = [
    "dollarmath",
    "amsmath",
    "colon_fence",
    "deflist",
]

# Breathe / Exhale (optional C++ API docs)
doxygen_xml_dir = _find_doxygen_xml_dir()
if doxygen_xml_dir is not None:
    extensions.extend(["breathe", "exhale"])
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

# General configuration
templates_path = ["_templates"]
# Only the html builder auto-excludes html_static_path; linkcheck and latex would read stray .md files there.
exclude_patterns = ["_static/**"]

# Linkcheck gates internal links only; external URLs flake (rate limits, outages).
linkcheck_ignore = [r"^https?://"]

# Graphviz
graphviz_output_format = "svg"

# The build runs with -W; Exhale documents nested types twice (own page and parent's), which no source edit avoids.
suppress_warnings = ["duplicate_declaration.cpp"]
