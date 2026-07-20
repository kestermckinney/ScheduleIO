# MppIO Class

`MppIO` is the public facade for binary Microsoft Project files. It reads MPP12/MPP14 files into a
[`schedule::Project`](../DataModel/Project.md) and writes native MPP14 files.

```cpp
#include "mppio.h"
```

The class is non-copyable (`Q_DISABLE_COPY`). Create one per file you want to read.

## Member functions

### `MppIO()` / `~MppIO()`

Construct and destroy an instance. A fresh instance holds an empty `schedule::Project`.

---

### `bool open(const QString &path)`

Read a `.mpp` file from disk into the internal project.

* **Returns** `true` on success, `false` on failure (file cannot be opened, not a valid `.mpp`
  container, or an unrecognised/unsupported format version).
* On failure, call [`errorString()`](#errorstring) for details.

```cpp
MppIO io;
if (!io.open("Schedule.mpp"))
    qWarning() << io.errorString();
```

---

### `bool openFromData(const QByteArray &bytes)`

Read a `.mpp` file from an in-memory byte array. Behaves exactly like `open()` but takes the file
contents directly — handy for files received over the network or embedded in another container.

* **Returns** `true` on success, `false` on failure.

---

### `bool save(const QString &path)`

Write the current project to `path`.

* `Mpp14` writes a real template-based binary MPP14 container (Project 2010 and later family).
* `Unknown` defaults to `Mpp14`, which is convenient for newly constructed projects.
* `Mpp12` uses the legacy ScheduleIO scaffold and should only be used for internal round trips; it
  is not a Microsoft Project-compatible MPP12 writer.

* **Returns** `true` on success, `false` on failure.

```cpp
schedule::Project edited = io.project();
edited.title = "Updated plan";

io.setProject(edited);
if (!io.save("Updated-plan.mpp"))
    qWarning() << io.errorString();
```

---

### `QByteArray saveToData()`

Like `save()`, but returns the complete compound-file image instead of writing a file. Returns an
empty array on failure.

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

Replace the current in-memory project by value. The caller may safely modify or destroy its source
object after this call. Use this before `save()` / `saveToData()` for a copied, edited, or newly
constructed project.

## Ownership and lifetime

`open()` reads the complete source file and closes it before returning. `openFromData()` does not
retain a reference to the caller's `QByteArray`. The reference from `project()` belongs to the
`MppIO` instance and remains valid until that instance is destroyed, `setProject()` is called, or a
later successful `open()` replaces the model.

See [MPP File Format and Storage](../Reference/FileFormat.md) for the container layout, stream
formats, read/write pipeline, and preservation boundaries.

---

### `QString errorString() const` { #errorstring }

Return a human-readable description of the most recent failure. Empty when the last operation
succeeded.

## C factory entry points

For run-time loading, the library also exports an `extern "C"` factory. See
[Dynamic Loading](../GettingStarted/DynamicLoading.md).

```cpp
extern "C" {
    MppIO      *scheduleio_create();
    void        scheduleio_destroy(MppIO *io);
    const char *scheduleio_version();
}
```
