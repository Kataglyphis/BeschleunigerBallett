# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

from pathlib import Path

# Project-local stylesheet, loaded AFTER the shared brand one so its rules win.
#
# css/custom.css is a SYMLINK to DocumANTation's generated brand stylesheet
# (see docs/source/_static/css/beschleunigerballett.css for the full story); it
# is generated from style/brand.json and must not be forked. Anything specific
# to this project goes in the file below instead, which is why every branch
# under here appends it - drop it from one branch and that branch silently
# renders without the project rules.
PROJECT_CSS_FILE = "css/beschleunigerballett.css"

# Import the shared Sphinx baseline from ANTfrastructure's vendored DocumANTation.
#
# The shared docs tooling lives in DocumANTation, which ANTfrastructure declares as
# a submodule at third_party/DocumANTation (see third_party/ANTfrastructure/
# .gitmodules) — not under external/, which holds only an untracked leftover of
# the pre-move layout. Get the path wrong and .exists() is silently False: the
# else-branch fallback below configures these docs instead, and the shared
# extension list and theme options are never applied.
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

        # Inherit shared extensions and theme settings
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


# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = "BeschleunigerBallett"
copyright = "2024, Jonas Heinle"
author = "Jonas Heinle"
release = (REPO_ROOT / "version.txt").read_text(encoding="utf-8").strip()

# -- Project-specific overrides ------------------------------------------------
# Update repository URL for this project
html_theme_options["repository_url"] = (
    "https://github.com/Kataglyphis/BeschleunigerBallett"
)

# -- Add project-specific extensions -------------------------------------------
extensions.extend(
    [
        "sphinx.ext.graphviz",
        "sphinx.ext.inheritance_diagram",
    ]
)

# -- Extra static payloads ------------------------------------------------------
# Copied verbatim into the site root: the WebGPU/glTF WASM demo (built from
# third_party/OxidANT/crates/webgpu_renderer; see
# docs/webgpu-gltf-rust-plan.md for the rebuild commands).
html_extra_path = ["_webgpu_demo"]

# -- MyST extension configuration -----------------------------------------------
myst_enable_extensions = [
    "dollarmath",
    "amsmath",
    "colon_fence",
    "deflist",
]

# -- Breathe / Exhale configuration (optional C++ API docs) --------------------
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
    # Exhale normally writes this page during the build. Without Doxygen XML,
    # exhale never runs, so README.md / index.rst link to a page that does not
    # exist and the html/linkcheck builds warn on it. Mirror graphviz_files.rst:
    # write a placeholder so the "optional, hand-written pages always build"
    # contract (documentation_workflow.md) holds for this page too.
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

# -- General configuration ---------------------------------------------------
templates_path = ["_templates"]
# The html builder auto-excludes html_static_path from document discovery,
# but other builders (linkcheck, latex, ...) do not - so a stray .md file
# under _static (e.g. VULKAN.md) is read as an orphan page there and warns.
# Exclude it explicitly so every builder agrees with the html builder.
exclude_patterns = ["_static/**"]

# -- linkcheck configuration ---------------------------------------------------
# `make linkcheck` gates broken pages/anchors/refs on OUR site (see
# documentation_workflow.md). External links flake (rate limits, transient
# outages, sites requiring a browser) and are not what this gate is for, so
# every external URL is excluded; only internal docs links/anchors are checked.
linkcheck_ignore = [r"^https?://"]

# -- Graphviz output format ---------------------------------------------------
graphviz_output_format = "svg"

# -- Warnings that are generated-docs artifacts, not documentation defects ----
# The docs build runs under `-W --keep-going` (ANTfrastructure's docs-build.sh), so
# every warning is fatal. That is the right default for the pages we write by
# hand; it is wrong for two things Exhale/Breathe do to the GENERATED API tree,
# which no edit on our side can influence:
#
#   duplicate_declaration.cpp - Exhale emits a nested type on its OWN page and
#     again inside its parent's page, so the C++ domain sees the declaration
#     twice. GpuTimingSubsystem::GpuPassAverage alone produced 8 of these. The
#     alternative would be to un-nest engine types to please a documentation
#     generator, which is the wrong way round.
#
# Everything else stays fatal. Notably NOT suppressed: toctree omissions, MyST
# xref failures, and doxygen*-directive resolution failures - each of those is
# a real documentation bug and each one was FIXED rather than silenced when the
# docs build was repaired on 2026-08-06 (53 warnings -> 0).
suppress_warnings = ["duplicate_declaration.cpp"]
