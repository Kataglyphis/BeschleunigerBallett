BeschleunigerBallett Documentation
==============================================

.. rst-class:: hero-section

Build and maintenance guide for BeschleunigerBallett.

- Project overview and repository map
- Build, run, packaging, and docs workflow
- Engine guides: shaders, model loading, path tracing, clouds, GPU golden tests
- Optional generated API and Graphviz reference material

.. grid:: 2
   :gutter: 2

   .. grid-item-card:: Project Overview
      :link: README
      :link-type: doc

      Renderer scope, repository layout and platform targets.

   .. grid-item-card:: Getting Started
      :link: getting_started
      :link-type: doc

      Clone, configure, build, test and run on Linux or Windows.

   .. grid-item-card:: Documentation Workflow
      :link: documentation_workflow
      :link-type: doc

      Build the Sphinx site, enable API docs, regenerate Graphviz content.

   .. grid-item-card:: Graphviz Architecture
      :link: graphviz_files
      :link-type: doc

      Open generated include graphs or follow the regeneration steps.

   .. grid-item-card:: API Reference
      :link: api/library_root
      :link-type: doc

      The C++ API entry page; with Doxygen XML available, the generated reference replaces it.

   .. grid-item-card:: WebGPU Demo (live)
      :link: webgpu_demo
      :link-type: doc

      Run the Rust WebGPU/glTF renderer in your browser: PBR, shadows, and ACES tonemapping via WebAssembly.

.. toctree::
   :maxdepth: 2
   :caption: Contents:
   :titlesonly:

   README.md
   getting_started.md
   documentation_workflow.md
   graphviz_files
   wsl2_vulkan
   webgpu_demo.md
   literature.md

.. toctree::
   :maxdepth: 1
   :caption: Engine guides

   shader-build-pipeline.md
   shader-sharing.md
   model-loading.md
   path-tracing.md
   clouds.md
   post-processing.md
   webgpu-srgb-audit.md
   gpu-golden-testing.md
   code-quality.md
   container-build-caching.md
   cpp-renderer-improvements.md
   LICENSES-README.md

.. toctree::
   :maxdepth: 1
   :caption: Moved to OxidANT

   webgpu-renderer-roadmap.md
   webgpu-gltf-rust-plan.md
   renderer-bounds-invariant.md

.. The Exhale-generated API tree. The card above links to it, but a card is not
   a toctree: without this entry Sphinx warns "document isn't included in any
   toctree" for api/library_root, which -W turns into a failed build. Hidden:
   the card is the entry point; this only gives the generated pages a parent.
.. toctree::
   :maxdepth: 2
   :hidden:

   api/library_root

Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
