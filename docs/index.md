# ScheduleIO

**ScheduleIO** is a cross-platform (Windows, macOS, Linux) C++17/Qt 6 library that reads Microsoft
Project `.mpp` files into Qt value types, edits and schedules the model, and writes native MPP14 or
MSPDI XML output. It has no dependency on an installed copy of Microsoft Project.

The `.mpp` format is undocumented; ScheduleIO decodes it directly — the OLE2 compound-document container,
the per-entity record streams, and the field maps that tie them together — and exposes the result as
a plain, copyable object model (`schedule::Project` and its child types).

[`MppIO`](API/MppIO.md) reads MPP12/MPP14 and writes native MPP14. [`XmlIO`](API/XmlIO.md) reads and
writes Microsoft Project compatible XML over the same model, so format conversion requires no
translation layer.

## What it reads

From a real `.mpp` file MppIO populates:

| Area | Fields |
| :--- | :--- |
| **Project** | metadata, start/finish/status dates, default calendar, format version, view styles |
| **Tasks** | identity/outline, dates, duration/work, actuals, constraints, scheduling mode, costs, baselines, earned value, custom fields, notes, formatting |
| **Resources** | identity, calendars, max units, availability, costs, rates, baselines, custom fields, notes |
| **Assignments** | task/resource links, span, delay, units, work/actuals, costs, baselines, custom fields, notes |
| **Relations** | predecessor links, type, lag and lag display unit |
| **Calendars** | base chains, weekly hours, exceptions, and resource-calendar links |

Every decoded field is cross-checked against Microsoft Project's own XML export in the test suite.
See [Field Coverage](Reference/FieldCoverage.md) for the validated detail.

## What it reads and writes as XML

[`XmlIO`](API/XmlIO.md) reads and writes the schedule-data fields as Microsoft Project compatible
XML (MSPDI). Reading XML, writing it back, and reading it again preserves those fields, and the output
imports cleanly into Microsoft Project. Binary-only view/row/cell formatting is not part of the XML
mapping. See [XML Interchange](GettingStarted/XmlInterchange.md).

## Key characteristics

* **Cross-platform** — a single Qt 6 / C++17 codebase for Windows, macOS, and Linux.
* **Self-contained** — parses `.mpp` directly; Microsoft Project is not required.
* **Value-type model** — every data structure is copyable and equality-comparable, so models can be
  compared, snapshotted, and round-tripped.
* **Dynamically loadable** — ships as a shared library with a C factory entry point, so a host
  application can load it at run time with `QLibrary` / `dlopen` / `LoadLibrary`.

## Where to go next

* [Building](GettingStarted/Building.md) — compile the library and run the tests.
* [Basic Usage](GettingStarted/BasicUsage.md) — open a file and read its contents.
* [Editing and Scheduling](GettingStarted/EditingAndScheduling.md) — construct, edit, recalculate,
  level, format, and save a project.
* [XML Interchange](GettingStarted/XmlInterchange.md) — read and write Microsoft Project XML.
* [Dynamic Loading](GettingStarted/DynamicLoading.md) — load the library at run time.
* [Data Model — Overview](DataModel/Overview.md) — how the object model is organised.
* [API Reference — MppIO Class](API/MppIO.md) — the binary `.mpp` facade.
* [API Reference — XmlIO Class](API/XmlIO.md) — the Microsoft Project XML facade.
* [Scheduling Utilities](API/Scheduling.md) — duration, calendar, scheduling, slack, and leveling APIs.
* [Project Reconciliation](API/Reconciliation.md) — authoritative work/cost buckets, rollups, and invariant checks.
* [MPP File Format and Storage](Reference/FileFormat.md) — the compound container, entity streams,
  field maps, ownership, and read/write pipelines.

## Status

MppIO reads real MPP12 and MPP14 files and writes template-based native MPP14 files. MPP12 output is
limited to ScheduleIO's legacy internal scaffold. The writer regenerates modeled entity streams and
supported view formatting from a stock MPP14 template; arbitrary unmodeled source records are not
preserved. See [Field Coverage](Reference/FieldCoverage.md) before using a read/edit/write cycle on a
file whose unsupported content must survive.
