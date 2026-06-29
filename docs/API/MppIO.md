# MppIO Class

`MppIO` is the public facade for the library. It reads a Microsoft Project `.mpp` file into an
[`MppProject`](../DataModel/MppProject.md) and gives access to the result.

```cpp
#include "mppio.h"
```

The class is non-copyable (`Q_DISABLE_COPY`). Create one per file you want to read.

## Member functions

### `MppIO()` / `~MppIO()`

Construct and destroy an instance. A fresh instance holds an empty `MppProject`.

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

!!! note
    `save()` writes the library's **own internal format**, not the Microsoft binary `.mpp` layout.
    It exists so a model can be round-tripped without loss. It does **not** produce a file that
    Microsoft Project can open.

* **Returns** `true` on success, `false` on failure.

---

### `QByteArray saveToData()`

Like `save()`, but returns the serialised bytes instead of writing a file. Returns an empty array on
failure.

---

### `const MppProject &project() const`

Return a reference to the current in-memory project — the result of the most recent successful
`open()` / `openFromData()`, or whatever was set with `setProject()`.

```cpp
const MppProject &p = io.project();
for (const MppTask &t : p.tasks)
    use(t);
```

---

### `void setProject(const MppProject &project)`

Replace the current in-memory project. Useful before `save()` / `saveToData()`, or to manipulate a
project the host constructed itself.

---

### `QString errorString() const` { #errorstring }

Return a human-readable description of the most recent failure. Empty when the last operation
succeeded.

## C factory entry points

For run-time loading, the library also exports an `extern "C"` factory. See
[Dynamic Loading](../GettingStarted/DynamicLoading.md).

```cpp
extern "C" {
    MppIO      *mppio_create();
    void        mppio_destroy(MppIO *io);
    const char *mppio_version();
}
```
