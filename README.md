<!-- Copyright (C) 2026 Paul McKinney -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ScheduleIO

A cross-platform (Windows / macOS / Linux) Qt 6/C++17 library for Microsoft Project schedules.
`MppIO` reads MPP12/MPP14 files and writes native template-based MPP14 files. `XmlIO` reads and
writes Microsoft Project compatible MSPDI XML over the same `schedule::Project` value model. The
library also provides working-calendar, dependency scheduling, critical-path, work/units, and
resource-leveling utilities. Microsoft Project does not need to be installed.

## Status

The full `open` → `schedule::Project` → edit/schedule → `save` pipeline is implemented:

- **Binary I/O** — a bounds-checked MS-CFB reader/writer plus decoded Props, field maps, fixed
  records, variable blobs, and MPP view-format data.
- **MPP14 writer** — regenerates tasks, resources, assignments, dependencies, calendars, project
  properties, and supported formatting from an embedded real-file template.
- **Model** — copyable Qt value types for schedule entities, costs, baselines, custom fields,
  actuals, availability, calendars, and formatting.
- **Scheduling** — working-time calendars, dependency forward/backward passes, critical path,
  Work = Duration x Units behavior, and resource leveling.
- **Validation** — low-level unit tests, semantic binary round trips, and oracle comparisons between
  real MPP fixtures and Microsoft Project XML exports.

MPP12 is readable; MPP12 output remains a ScheduleIO-only legacy scaffold. A binary save starts from
the embedded MPP14 template rather than patching the source file in place, so unsupported source-only
content is not guaranteed to survive. See the field-coverage and format/storage references below.

## Build

```sh
cmake -S . -B build -DSCHEDULEIO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires Qt 6 (Core + Test). On Windows, configure with the Qt/MSVC kit (e.g. via
Qt Creator) or pass `-DCMAKE_PREFIX_PATH=<Qt>/<ver>/msvc2022_64`.

## Testing strategy

Run the complete suite with `ctest --test-dir build --output-on-failure`. Real `.mpp` + `.xml`
fixture pairs under `tests/fixtures/` support cross-format oracle tests; generated fixtures cover
focused formatting and scheduling cases.

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
structures are under **Data Model**, the I/O and scheduling APIs under **API Reference**, and complete
examples under **Getting Started**. The detailed binary layout, compound storage behavior, ownership,
and writer pipeline are under **Reference → MPP File Format and Storage**; preservation details are
under **Reference → Field Coverage**.
