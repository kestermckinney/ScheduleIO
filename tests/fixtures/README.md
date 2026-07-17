<!-- Copyright (C) 2026 Paul McKinney -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Test fixtures

Drop **real Microsoft Project files** here as ground truth for the round-trip
(Layer 2) and oracle (Layer 3) tests. They are tracked via Git LFS (see
`../../.gitattributes`).

For each project, provide a pair:

| File             | Purpose                                                        |
|------------------|----------------------------------------------------------------|
| `<name>.mpp`     | the binary file the parser must read and round-trip            |
| `<name>.xml`     | MS Project "Save As XML" export of the same file — Layer 3 oracle |

Aim for a corpus covering:

- `empty.mpp` — a brand-new empty project
- `tasks-small.mpp` — a handful of tasks only
- `full.mpp` — tasks + resources + assignments + calendars + constraints + custom outline codes
- `large.mpp` — thousands of tasks (performance/scale)
- both **MPP.12** (Project 2007) and **MPP.14** (Project 2010+) format versions

Every fixture-driven test discovers `*.mpp`/`*.xml` **recursively** under this
directory (see `tests/fixtureutils.h`), so sample sets can live in their own
subfolders — e.g. the generated corpus under `mpp_samples/` (see its README).
Folders may also carry a `manifest.json` recording per-row text formatting;
`tst_format_oracle` verifies the decoded model against it, since the XML
export never contains presentation data.

## Third-party fixtures

- `mpp14availability.mpp` / `.xml` — sourced from
  [joniles/mpxj](https://github.com/joniles/mpxj) (`junit/data/mpp14availability.mpp`,
  `junit/data/mspdiavailability.xml` renamed to match this project's `<name>.mpp`/
  `<name>.xml` pairing convention), LGPL-2.1. The only fixture with real resource
  Availability-table data (see `AvailabilityTest.java` in that repo). Its
  `TBkndCal` calendar records use a layout our calendar reader doesn't fully
  parse yet — `tst_entities_oracle` knowingly skips calendar-identity checks for
  this one file (see `knownCalendarLayoutGap` in that test) rather than loosen
  them for every fixture.
