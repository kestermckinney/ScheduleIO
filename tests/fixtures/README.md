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

`tst_semantic_roundtrip` automatically discovers every `*.mpp` in this directory.
Until the binary MPP field mapping is reverse-engineered, those cases self-skip
with a clear message rather than failing.
