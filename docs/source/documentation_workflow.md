# Documentation Workflow

## Documentation Layers

The documentation setup has three distinct layers:

1. hand-written pages in `docs/source/`
2. optional C++ API reference generated from Doxygen XML through Breathe and Exhale
3. optional Graphviz include diagrams rendered from SVG files under `docs/source/_static/`

The hand-written pages should always build, even if generated artifacts are missing.

## Build the HTML Docs Locally

### Windows

```pwsh
.\.venv\scripts\python.exe -m pip install -r requirements.txt
.\.venv\scripts\python.exe -m sphinx -M html docs/source docs/build -E
```

### Linux

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python -m sphinx -M html docs/source docs/build -E
```

The generated HTML site ends up in `docs/build/html`.

## Optional C++ API Reference

The Sphinx configuration auto-enables the API reference when it finds a Doxygen XML directory containing `index.xml` in one of these places:

- the path from `KATAGLYPHIS_DOXYGEN_XML_DIR`
- `build/build/xml`
- `build/xml`
- `build-clangcl-debug/xml`
- `build-clangcl-release/xml`
- `build-clangcl-profile/xml`

If none of these locations exists, `conf.py` writes a placeholder `api/library_root.rst` that says so, and the rest of the docs still builds.

## Graphviz Include Graphs

`scripts/linux/docs-build-web.sh` automates the Graphviz flow:

1. ensure the Python environment and dependencies
2. copy generated SVG files into `docs/source/_static/`
3. run `docs/source/graphviz_generator.py`
4. build the HTML docs

You can also regenerate the page manually after SVG files are available:

```bash
cd docs/source
python graphviz_generator.py
```

If no SVGs are present, the generated page stays as a helpful placeholder instead of failing the docs build.

## Authoring Guidelines

- Keep the repository root README focused on orientation and quick-start information.
- Keep Sphinx pages task-focused and stable.
- Prefer relative links inside the docs tree.
- Treat generated artifacts as optional unless a page is explicitly about generated reference material.
- When changing build or packaging flows, update both the root README and the affected Sphinx page in the same change.