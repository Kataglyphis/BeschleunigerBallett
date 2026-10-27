# Documentation Workflow

## Documentation Layers

1. hand-written pages in `docs/source/`
2. optional C++ API reference generated from Doxygen XML through Breathe and Exhale
3. optional Graphviz include diagrams rendered from SVG files under `docs/source/_static/`

The hand-written pages must build even when the generated artifacts are missing.

## Build the HTML Docs Locally

### Windows

```pwsh
uv sync --locked
.\.venv\scripts\python.exe -m sphinx -M html docs/source docs/build -E
```

### Linux

```bash
uv sync --locked
uv run --locked python -m sphinx -M html docs/source docs/build -E
```

The site lands in `docs/build/html`.

## Publishing

CI builds the site in the x64 `release` job and publishes it through the hub's `build-docs.yml`, from
a push to `develop` or `main` only ([AGENTS.md § The docs site](../../AGENTS.md#the-docs-site)). The
hub's link gate fails the publish on any dead internal link or anchor in the built tree; run it on a
local build first:

```bash
python3 third_party/ANTfrastructure/linux/scripts/verify_site_links.py docs/build/html
```

## Theme and links

`conf.py` calls the family theme's `setup_theme()` (`sphinx_kataglyphis`, an editable install from
the hub's nested DocumANTation, so run `git submodule update --init --recursive` first). It brings
the brand CSS, the logo and favicon, and the link handling: a Markdown link to a page, with or
without a `#heading`, resolves to the built page as it does on GitHub, and a link to a repository
file the site does not publish goes to that file on GitHub. Rules only this site has go into
`_static/css/beschleunigerballett.css`, in brand tokens. What the theme does:
[`sphinx-book/README.md`](https://github.com/Kataglyphis/DocumANTation/blob/develop/docs-tooling/source_templates/sphinx-book/README.md).

## Optional C++ API Reference

`conf.py` enables the API reference when it finds a Doxygen XML directory containing `index.xml` in one of these places:

- the path from `KATAGLYPHIS_DOXYGEN_XML_DIR`
- `build/build/xml`
- `build/xml`
- `build-clangcl-debug/xml`
- `build-clangcl-release/xml`
- `build-clangcl-profile/xml`

Without one, `conf.py` writes a placeholder `api/library_root.rst` that says so, and the rest of the docs still builds.

## Graphviz Include Graphs

`scripts/linux/docs-build-web.sh` automates the Graphviz flow:

1. ensure the Python environment and dependencies
2. copy generated SVG files into `docs/source/_static/`
3. run `docs/source/graphviz_generator.py`
4. build the HTML docs

By hand, once the SVG files are in place:

```bash
cd docs/source
python graphviz_generator.py
```

With no SVGs the generated page is a placeholder; the docs build does not fail.

## Authoring Guidelines

- Keep the repository root README a short overview: what the project is, the quick start, and one row per guide ([the family's README rule](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/repository-family.md#the-readme-rule)). Everything else has a page here.
- Keep Sphinx pages task-focused and stable.
- Prefer relative links inside the docs tree.
- Treat generated artifacts as optional unless a page is about generated reference material.
- A change to a build or packaging flow updates the affected Sphinx page in the same change, and the root README only when its quick start or highlights change.
