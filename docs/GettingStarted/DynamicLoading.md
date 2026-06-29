# Dynamic Loading

MppIO is built as a shared library and is designed to be loaded at **run time** — a host application
can resolve and use it without linking against it at build time. This is useful for optional features,
plugin architectures, or keeping the host's build free of the dependency.

The library exposes a small `extern "C"` factory so the symbols are easy to resolve across compilers:

```cpp
extern "C" {
    MppIO       *mppio_create();          // construct an instance
    void         mppio_destroy(MppIO *);  // destroy it
    const char  *mppio_version();         // library version string, e.g. "0.1.0"
}
```

## Loading with QLibrary

```cpp
#include "mppio.h"
#include <QLibrary>

QLibrary lib("MppIO");          // resolves MppIO.dll / libMppIO.so / libMppIO.dylib
if (!lib.load()) {
    qWarning() << lib.errorString();
    return;
}

using CreateFn  = MppIO *(*)();
using DestroyFn = void (*)(MppIO *);
using VersionFn = const char *(*)();

auto create  = reinterpret_cast<CreateFn>(lib.resolve("mppio_create"));
auto destroy = reinterpret_cast<DestroyFn>(lib.resolve("mppio_destroy"));
auto version = reinterpret_cast<VersionFn>(lib.resolve("mppio_version"));

qInfo() << "MppIO version" << version();

MppIO *io = create();
if (io->open("Schedule.mpp"))
    process(io->project());
destroy(io);

lib.unload();
```

Because the factory returns a real `MppIO *`, you call its member functions normally once you have an
instance — you only need the resolved symbols to construct and destroy it.

## Notes

* The library file name is `MppIO` to `QLibrary`; the OS-specific prefix/suffix
  (`.dll`, `lib…​.so`, `lib…​.dylib`) is added for you.
* Make sure the library is on the loader search path: the same directory as the host executable, a
  directory on `PATH` (Windows) / `LD_LIBRARY_PATH` (Linux) / `DYLD_LIBRARY_PATH` (macOS), or pass an
  absolute path to `QLibrary`.
* The Qt runtime the library was built against must also be available at load time.
