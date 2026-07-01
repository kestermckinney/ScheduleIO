# ScheduleIO

**ScheduleIO** is a cross-platform (Windows, macOS, Linux) C++ library, built with **Qt 6**, that reads
Microsoft Project `.mpp` files into Qt data structures so you can inspect and manipulate a project
schedule in code. It is built as a **dynamically loaded** shared library and has no dependency on an
installed copy of Microsoft Project.

The `.mpp` format is undocumented; ScheduleIO decodes it directly — the OLE2 compound-document container,
the per-entity record streams, and the field maps that tie them together — and exposes the result as
a plain, copyable object model (`schedule::Project` and its child types).

The library also includes [`XmlIO`](API/XmlIO.md), which reads **and writes** Microsoft Project
compatible XML (the MSPDI `.xml` format) over the very same object model. So you can read a binary
`.mpp` with `MppIO` and save standard XML that Microsoft Project can open with `XmlIO` — see
[XML Interchange](GettingStarted/XmlInterchange.md).

## What it reads

From a real `.mpp` file MppIO populates:

| Area | Fields |
| :--- | :--- |
| **Project** | title, author, start date, finish date, format version |
| **Tasks** | unique id, name, ID, WBS, outline level, start, finish, duration, percent complete, milestone, summary, constraint type/date |
| **Resources** | unique id, ID, name, initials, max units |
| **Assignments** | task ↔ resource links, units, work |
| **Relations** | predecessor links (with link type) |
| **Calendars** | unique id, name, base calendar, working-day mask |

Every decoded field is cross-checked against Microsoft Project's own XML export in the test suite.
See [Field Coverage](Reference/FieldCoverage.md) for the validated detail.

## What it reads and writes as XML

[`XmlIO`](API/XmlIO.md) reads and writes the same fields as Microsoft Project compatible XML
(MSPDI). Reading a `.xml`, writing it back, and reading it again yields an identical model, and the
output imports cleanly into Microsoft Project. See [XML Interchange](GettingStarted/XmlInterchange.md).

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
* [XML Interchange](GettingStarted/XmlInterchange.md) — read and write Microsoft Project XML.
* [Dynamic Loading](GettingStarted/DynamicLoading.md) — load the library at run time.
* [Data Model — Overview](DataModel/Overview.md) — how the object model is organised.
* [API Reference — MppIO Class](API/MppIO.md) — the binary `.mpp` facade.
* [API Reference — XmlIO Class](API/XmlIO.md) — the Microsoft Project XML facade.

## Status

MppIO reads a wide range of project, task, resource, assignment, relation, and calendar fields, and
`XmlIO` reads **and writes** those same fields as Microsoft Project compatible XML (MSPDI). Writing
the binary `.mpp` format back out is **not** supported — like every tool other than Microsoft Project
itself, the library does not produce binary `.mpp` files. (`MppIO::save()` exists for round-tripping
the library's own internal format; it does not write Microsoft's binary layout.) When you need a file
Microsoft Project can open, write `.xml` with [`XmlIO`](API/XmlIO.md).
