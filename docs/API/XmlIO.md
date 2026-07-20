# XmlIO Class

`XmlIO` is the public facade for reading and writing **Microsoft Project compatible XML** — the
MSPDI format (namespace `http://schemas.microsoft.com/project`) that Microsoft Project itself
exports and imports as `.xml`. It reads such a file into an
[`schedule::Project`](../DataModel/Project.md) and writes one back out.

```cpp
#include "xmlio.h"
```

`XmlIO` shares the **exact same object model** as [`MppIO`](MppIO.md): both populate an
`schedule::Project`. That means you can load a project from a binary `.mpp` with `MppIO` and save it as
`.xml` with `XmlIO` (or the reverse) with no translation step in between — see
[XML Interchange](../GettingStarted/XmlInterchange.md).

The class is non-copyable (`Q_DISABLE_COPY`). Create one per file you want to read or write.

!!! note "How it differs from `MppIO::save()`"
    `XmlIO::save()` writes standard, text-based MSPDI. [`MppIO::save()`](MppIO.md) writes a native
    binary MPP14 file when the model's format is `Mpp14` (or `Unknown`). Both outputs can be consumed
    by Microsoft Project; choose XML for transparent interchange and MPP14 for native binary output.

## Member functions

### `XmlIO()` / `~XmlIO()`

Construct and destroy an instance. A fresh instance holds an empty `schedule::Project`.

---

### `bool open(const QString &path)`

Read an MSPDI `.xml` file from disk into the internal project.

* **Returns** `true` on success, `false` on failure (file cannot be opened, not well-formed XML, or
  the root element is not `<Project>`).
* On failure, call [`errorString()`](#errorstring) for details.

```cpp
XmlIO io;
if (!io.open("Schedule.xml"))
    qWarning() << io.errorString();
```

---

### `bool openFromData(const QByteArray &bytes)`

Read an MSPDI document from an in-memory byte array. Behaves exactly like `open()` but takes the
document contents directly — handy for files received over the network or generated in memory.

* **Returns** `true` on success, `false` on failure.

---

### `bool save(const QString &path)`

Write the current project to `path` as a Microsoft Project compatible MSPDI document.

* **Returns** `true` on success, `false` on failure (the file cannot be written).
* The output is UTF-8, indented, and uses the `http://schemas.microsoft.com/project` namespace, so
  Microsoft Project will import it.

```cpp
XmlIO io;
io.setProject(project);
if (!io.save("Schedule.xml"))
    qWarning() << io.errorString();
```

---

### `QByteArray saveToData()`

Like `save()`, but returns the serialised XML bytes instead of writing a file. Returns an empty
array only on a genuine failure (an empty project still produces a valid document).

---

### `const schedule::Project &project() const`

Return a reference to the current in-memory project — the result of the most recent successful
`open()` / `openFromData()`, or whatever was set with `setProject()`.

```cpp
const schedule::Project &p = io.project();
for (const schedule::Task &t : p.tasks)
    use(t);
```

---

### `void setProject(const schedule::Project &project)`

Replace the current in-memory project. Call this before `save()` / `saveToData()` — for example with
a project read from a `.mpp` via `MppIO`, or one the host built itself.

---

### `QString errorString() const` { #errorstring }

Return a human-readable description of the most recent failure. Empty when the last operation
succeeded.

## What is read and written

`XmlIO` maps the schedule-data portion of the [data model](../DataModel/Overview.md):

| Area | MSPDI ↔ model |
| :--- | :--- |
| **Project** | `SaveVersion`, `Title`, `Author`, `StartDate`, `FinishDate`, `StatusDate`, `CalendarUID` |
| **Tasks** | identity/outline, scheduled and actual dates, duration/work, completion, active/manual/task type, effort-driven, priority, deadline, calendar, constraints, notes, costs, earned value |
| **Baselines** | task `<Baseline>` (number, start, finish, duration, work, cost) |
| **Predecessor links** | `<PredecessorLink>` on the successor task ↔ `schedule::Project.relations` |
| **Extended attributes** | `<ExtendedAttribute>` ↔ `customFields` (decoded to the field's natural Qt type) |
| **Resources** | UID, ID, name/initials, max units, calendar, availability periods, costs, notes, and rate tables |
| **Assignments** | identity/links, scheduled span, delay/leveling delay, units, work/actual/remaining work, costs, notes |
| **Calendars** | `WeekDays` (working-day mask + working times) and `Exceptions` |

Durations and work are written as ISO-8601 durations (e.g. `PT8H0M0S`), preserving Microsoft
Project's sub-second precision. See [Field Coverage](../Reference/FieldCoverage.md).

MPP view styles, row/cell formatting, font-base payloads, per-task bar colors, and the report accent
are not serialized by `XmlIO`; use MPP14 when those supported binary presentation fields must be
retained.

## C factory entry points

For run-time loading, the library also exports an `extern "C"` factory alongside MppIO's. See
[Dynamic Loading](../GettingStarted/DynamicLoading.md).

```cpp
extern "C" {
    XmlIO      *xmlio_create();
    void        xmlio_destroy(XmlIO *io);
    const char *xmlio_version();
}
```
