# Building

MppIO is a CMake project that targets **Qt 6** (Qt 5 is accepted as a fallback) and **C++17**.

## Prerequisites

* **Qt 6** with the `Core` module (and `Test` if you build the test suite).
* **CMake** 3.16 or newer.
* A C++17 compiler:
    * Windows — MSVC 2022 (x64).
    * macOS — Apple Clang.
    * Linux — GCC or Clang.

## Configure and build

```sh
cmake -S . -B build -DMPPIO_BUILD_TESTS=ON
cmake --build build
```

On Windows, build from a Qt/MSVC environment (for example a Qt Creator kit, or a
*Developer Command Prompt*), or pass the Qt prefix explicitly:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.10.0/msvc2022_64 \
    -DMPPIO_BUILD_TESTS=ON
cmake --build build
```

## Build options

| Option | Default | Purpose |
| :--- | :--- | :--- |
| `MPPIO_BUILD_TESTS` | `ON` | Build the QtTest suite and register it with CTest. |
| `MPPIO_ORACLE_MSPROJECT` | `OFF` | Reserved for the optional Windows-only writer-acceptance oracle. |

## Targets

| Target | Type | Description |
| :--- | :--- | :--- |
| `MppIO` | `SHARED` library | The library itself (`MppIO.dll` / `libMppIO.so` / `libMppIO.dylib`). |
| `mppio_objects` | `OBJECT` library | Internal — all sources compiled once; the shared library and the unit tests both consume it. |

## Running the tests

The test suite cross-checks decoded data against Microsoft Project XML exports. Layer 1 (unit) tests
run with no external data; the Layer 3 oracle tests use real `.mpp` + `.xml` fixture pairs placed in
`tests/fixtures/`.

```sh
ctest --test-dir build --output-on-failure
```

Make sure the Qt runtime and the built `MppIO` shared library are on the library search path when
running the tests (on Windows, add the Qt `bin` directory and the build directory to `PATH`).

## Linking against MppIO

If you build with CMake, add the library subdirectory and link the target:

```cmake
add_subdirectory(MppIO)            # or add_subdirectory(../MppIO MppIO)
target_link_libraries(your_app PRIVATE MppIO)
```

The `MppIO` target exports its include directory, so `#include "mppio.h"` resolves automatically.
Genuine downstream consumers that link the shared library import its symbols via the `MPPIO_DLL`
interface definition. If you prefer to load the library at run time instead of linking it, see
[Dynamic Loading](DynamicLoading.md).
