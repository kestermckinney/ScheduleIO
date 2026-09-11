<!-- Copyright (C) 2026 Paul McKinney -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ScheduleIO

[![Documentation](https://img.shields.io/badge/Documentation-GitHub%20Pages-2ea44f?style=for-the-badge)](https://kestermckinney.github.io/ScheduleIO/)
[![Latest release](https://img.shields.io/github/v/release/kestermckinney/ScheduleIO?style=for-the-badge)](https://github.com/kestermckinney/ScheduleIO/releases)
[![Stars](https://img.shields.io/github/stars/kestermckinney/ScheduleIO?style=for-the-badge)](https://github.com/kestermckinney/ScheduleIO/stargazers)
[![Open issues](https://img.shields.io/github/issues/kestermckinney/ScheduleIO?style=for-the-badge)](https://github.com/kestermckinney/ScheduleIO/issues)
[![Last commit](https://img.shields.io/github/last-commit/kestermckinney/ScheduleIO?style=for-the-badge)](https://github.com/kestermckinney/ScheduleIO/commits/main)

ScheduleIO is a cross-platform Qt/C++ library for applications that need to read, create, edit,
schedule, or convert Microsoft Project plans without automating an installed copy of Microsoft
Project. `MppIO` handles MPP12/MPP14 binary files, `XmlIO` handles Microsoft Project-compatible
MSPDI XML, and both use the same copyable `schedule::Project` value model.

## Explore

- [Documentation and build guide](docs/index.md)
- [Data model reference](docs/DataModel/Overview.md)
- [Field coverage and preservation limits](docs/Reference/FieldCoverage.md)
- [Report a bug or request a feature](https://github.com/kestermckinney/ScheduleIO/issues/new)
- [Contributing guide](CONTRIBUTING.md)

## What ScheduleIO Does

- Reads MPP12 and MPP14 compound documents directly on Windows, macOS, and Linux.
- Writes native, template-based MPP14 files and reads/writes MSPDI XML.
- Exposes tasks, resources, assignments, dependencies, calendars, baselines, costs, custom fields,
  time-phased data, project options, and view formatting as plain Qt value types.
- Provides working-calendar arithmetic, dependency scheduling, critical-path analysis,
  work/duration/units recalculation, progress updates, cost reconciliation, and resource leveling.
- Supports normal CMake linking and optional run-time loading through stable C factory functions.
- Requires no Microsoft Project installation for normal library use.

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

## Quick Build

```sh
git clone https://github.com/kestermckinney/ScheduleIO.git
cd ScheduleIO
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSCHEDULEIO_BUILD_TESTS=OFF
cmake --build build --config Release --parallel
```

The library requires CMake 3.16+, a C++17 compiler, and Qt 6 Core. Qt 5 is accepted as a fallback.
Tests additionally require Qt Test. Windows users normally provide the Qt kit with
`-DCMAKE_PREFIX_PATH=C:/Qt/<version>/msvc2022_64` or configure the project in Qt Creator.

See the [complete build guide](docs/GettingStarted/Building.md)
for Linux, macOS, Windows, tests, consumer integration, output locations, and run-time deployment.

## Testing strategy

Run the complete suite with `ctest --test-dir build --output-on-failure`. Real `.mpp` + `.xml`
fixture pairs under `tests/fixtures/` support cross-format oracle tests; generated fixtures cover
focused formatting and scheduling cases.

## Data Model

One `schedule::Project` owns flat lists of tasks, resources, assignments, calendars, and relations.
Integer unique IDs connect those lists; nested value types hold baselines, rates, availability,
time-phased buckets, custom fields, and presentation settings. Dates use Qt date/time types,
duration and work values use milliseconds, percentages and units use ratios (`1.0 == 100%`), and
currency values are stored as ordinary amounts in the project's currency.

The [data model overview](docs/DataModel/Overview.md) explains
ownership, links, units, sentinels, equality, persistence, and every public model structure.

## Activity And Request Snapshot

[![Open bugs](https://img.shields.io/github/issues-search?query=repo%3Akestermckinney%2FScheduleIO+is%3Aissue+is%3Aopen+label%3Abug&label=open%20bugs&style=flat-square)](https://github.com/kestermckinney/ScheduleIO/issues?q=is%3Aissue%20is%3Aopen%20label%3Abug)
[![Open enhancements](https://img.shields.io/github/issues-search?query=repo%3Akestermckinney%2FScheduleIO+is%3Aissue+is%3Aopen+label%3Aenhancement&label=open%20enhancements&style=flat-square)](https://github.com/kestermckinney/ScheduleIO/issues?q=is%3Aissue%20is%3Aopen%20label%3Aenhancement)

These badges update from GitHub and give visitors a quick view of project activity and open work.

## Documentation

Programmer documentation (usage, the `MppIO` / `XmlIO` API, and the data model) is written with
[MkDocs](https://www.mkdocs.org/) under `docs/`, and is configured for Read the Docs
(`.readthedocs.yaml`).

Preview it locally:

```sh
pip install mkdocs
mkdocs serve          # then open http://127.0.0.1:8000/
```

Or build the static site with `mkdocs build --strict` (output in `site/`). GitHub Actions publishes
that same site to GitHub Pages after changes land on `main`.

## Contributing

Bug reports, compatibility fixtures, focused code changes, tests, and documentation improvements are
welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. When reporting a
format problem, include the ScheduleIO revision, platform, file format/version, exact operation, and
a minimized non-sensitive fixture when possible.
