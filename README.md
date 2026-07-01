<!-- Copyright (C) 2026 Paul McKinney -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ScheduleIO

A cross-platform (Windows / macOS / Linux) Qt6 C++ library that reads Microsoft
Project `.mpp` files into Qt data structures for manipulation. It also reads **and
writes** Microsoft Project compatible XML (the MSPDI `.xml` format) over the same
object model via the `XmlIO` class — so you can load a `.mpp` and save XML that
Microsoft Project can open. Built as a **dynamically loaded** shared library.

## Status

Scaffold. The full pipeline (`open` → `schedule::Project` → `save`) works end to end on
a self-consistent encoding, and the lower layers are real:

- **`src/ole/compoundfile`** — a working [MS-CFB] (OLE2 compound document)
  reader/writer (FAT, mini-FAT, directory BST), the same container `.mpp` uses.
- **`src/codec/fielddecoders`**, **`src/codec/streamquartet`** — reversible field
  codecs and the `FixedMeta`/`VarMeta`/`FixedData`/`Var2Data` quartet model.
- **`src/model/*`** — the Qt data structures (value types with `operator==`).
- **`src/serializer/*`** — version dispatch (MPP.12 / MPP.14).

**To be reverse-engineered against real fixtures:** the exact on-disk MPP field
layouts inside the quartet streams. That seam is marked in
`src/serializer/docserializer.cpp`. See `tests/fixtures/README.md`.

## Build

```sh
cmake -S . -B build -DSCHEDULEIO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires Qt 6 (Core + Test). On Windows, configure with the Qt/MSVC kit (e.g. via
Qt Creator) or pass `-DCMAKE_PREFIX_PATH=<Qt>/<ver>/msvc2022_64`.

## Testing strategy

See the layered strategy in the project plan. Layers 1 (unit), 5 (dynamic load)
and the synthetic round-trip run with no external data; Layers 2–3 activate once
real `.mpp` + `.xml` fixture pairs are added under `tests/fixtures/`.

## Documentation

Programmer documentation (usage, the `MppIO` / `XmlIO` API, and the data model) is written with
[MkDocs](https://www.mkdocs.org/) under `docs/`, and is configured for Read the Docs
(`.readthedocs.yaml`).

Preview it locally:

```sh
pip install mkdocs
mkdocs serve          # then open http://127.0.0.1:8000/
```

Or build the static site with `mkdocs build` (output in `site/`). Start at `docs/index.md`; the data
structures are under **Data Model**, the public classes (`MppIO` and `XmlIO`) under **API Reference**,
reading/writing Microsoft Project XML under **Getting Started → XML Interchange**, and what is/isn't
decoded under **Reference → Field Coverage**.
