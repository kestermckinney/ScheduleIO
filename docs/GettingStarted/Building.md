# Building

ScheduleIO is a CMake project that targets **Qt 6** (Qt 5 is accepted as a fallback) and **C++17**.

This page covers building the shared library itself, running its tests, and consuming the CMake
target from another application. ScheduleIO currently exposes a build-tree CMake target; it does
not yet install a package configuration with `cmake --install`.

## Prerequisites

* **Qt 6** with the `Core` module. Qt 5 is accepted if Qt 6 is not available.
* **Qt Test** only when `SCHEDULEIO_BUILD_TESTS=ON`.
* **CMake** 3.16 or newer.
* A C++17 compiler:
    * Windows — MSVC 2022 (x64).
    * macOS — Apple Clang.
    * Linux — GCC or Clang.

Keep the Qt library and your application on the same Qt major version, compiler toolchain, CPU
architecture, and C/C++ runtime. This is especially important for a shared C++ library whose public
API contains Qt types.

## Clone the source

```sh
git clone https://github.com/kestermckinney/ScheduleIO.git
cd ScheduleIO
```

For a reproducible application build, check out a release tag or pin a specific commit instead of
following `main` indefinitely.

## Build only the library

Use this path when embedding ScheduleIO in an application and you do not need its test executables:

```sh
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DSCHEDULEIO_BUILD_TESTS=OFF
cmake --build build --config Release --parallel
```

`CMAKE_BUILD_TYPE` is used by single-configuration generators such as Unix Makefiles and Ninja.
`--config Release` selects the configuration for Visual Studio, Xcode, and Ninja Multi-Config; it is
harmless with common single-configuration generators.

The result is the `ScheduleIO` shared library:

| Platform | Typical output |
| :--- | :--- |
| Linux | `build/libScheduleIO.so` (plus versioned symlinks) |
| macOS | `build/libScheduleIO.dylib` |
| Windows | `build/Release/ScheduleIO.dll` and the matching import library |

The exact directory varies by generator. `cmake --build build --target ScheduleIO --config Release`
builds only the library target.

## Build and run the tests

```sh
cmake -S . -B build-tests \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSCHEDULEIO_BUILD_TESTS=ON
cmake --build build-tests --config RelWithDebInfo --parallel
ctest --test-dir build-tests -C RelWithDebInfo --output-on-failure
```

The ordinary test suite is cross-platform and needs no Microsoft Project installation. Real `.mpp`
and matching `.xml` fixtures under `tests/fixtures/` drive format-oracle tests. The optional
`SCHEDULEIO_ORACLE_MSPROJECT` test is Windows-only and requires Microsoft Project.

## Point CMake at Qt

If CMake cannot find Qt, pass the Qt installation prefix. The directory must contain Qt's CMake
package files, normally beneath the compiler-specific kit directory.

### Windows

Use the same Qt/MSVC kit that will build the consuming application. From a Developer Command Prompt
or ordinary terminal with CMake and Ninja available:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/msvc2022_64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSCHEDULEIO_BUILD_TESTS=OFF
cmake --build build --parallel
```

With the Visual Studio generator, use `cmake --build build --config Release`. MinGW users should
select a MinGW Qt kit and a matching MinGW compiler instead of mixing it with an MSVC-built Qt.

### macOS

For a Qt installation whose prefix is available from `qtpaths`, configure with:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH="$(qtpaths --query QT_INSTALL_PREFIX)" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSCHEDULEIO_BUILD_TESTS=OFF
cmake --build build --parallel
```

Use an arm64 Qt kit for Apple Silicon and an x86_64 kit for Intel builds. A universal binary
requires Qt libraries built for both architectures.

### Linux

Install Qt's development files, CMake, and a C++ compiler using your distribution's package manager,
then run the standard library-only or test commands above. If Qt is installed outside the normal
system prefixes, set `CMAKE_PREFIX_PATH` to that Qt installation.

### Qt Creator

Open the repository's top-level `CMakeLists.txt`, select a desktop Qt kit, and configure. Add
`SCHEDULEIO_BUILD_TESTS=OFF` under **Projects > Build > CMake** for a library-only build. Build the
`ScheduleIO` target; enable tests when working on the library itself.

## Build options

| Option | Default | Purpose |
| :--- | :--- | :--- |
| `SCHEDULEIO_BUILD_TESTS` | `ON` | Build the QtTest suite and register it with CTest. |
| `SCHEDULEIO_ORACLE_MSPROJECT` | `OFF` | Enables the Windows-only writer-acceptance oracle. It launches an installed Microsoft Project instance, verifies native Usage-view widths, resaves the MPP, and checks the resaved settings with ScheduleIO. |

## Targets

| Target | Type | Description |
| :--- | :--- | :--- |
| `ScheduleIO` | `SHARED` library | The library itself (`ScheduleIO.dll` / `libScheduleIO.so` / `libScheduleIO.dylib`). |
| `scheduleio_objects` | `OBJECT` library | Internal — all sources compiled once; the shared library and the unit tests both consume it. |

## Linking against ScheduleIO

The supported integration path is to make the ScheduleIO source tree part of your CMake build. For a
vendored checkout or Git submodule:

```cmake
set(SCHEDULEIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ScheduleIO)
target_link_libraries(your_app PRIVATE ScheduleIO)
```

You can also keep the repositories side by side:

```cmake
set(SCHEDULEIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../ScheduleIO ScheduleIO-build)
target_link_libraries(your_app PRIVATE ScheduleIO)
```

The target propagates the public include directories, Qt Core dependency, and `SCHEDULEIO_DLL`
consumer definition, so application code can simply include the facade:

```cpp
#include "mppio.h"
#include "xmlio.h"
```

Model-specific headers are available beneath `src/model`, for example
`#include "model/project.h"` or `#include "model/duration.h"`.

## FetchContent integration

CMake can fetch a pinned release or commit during configuration:

```cmake
include(FetchContent)

set(SCHEDULEIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    scheduleio
    GIT_REPOSITORY https://github.com/kestermckinney/ScheduleIO.git
    GIT_TAG        <release-tag-or-full-commit>
)
FetchContent_MakeAvailable(scheduleio)

target_link_libraries(your_app PRIVATE ScheduleIO)
```

Replace the placeholder with a reviewed tag or full commit hash. Pinning avoids silently changing
your dependency when `main` advances.

## Run-time deployment

The built library depends on Qt Core. At run time:

* Windows needs `ScheduleIO.dll`, the matching Qt Core DLL, and the compiler runtime where the
  executable loader can find them. Qt's `windeployqt` can stage Qt dependencies.
* macOS applications normally bundle the dylib and Qt frameworks; `macdeployqt` can prepare an app
  bundle.
* Linux applications need the ScheduleIO and Qt shared libraries in the loader search path, an
  application-specific rpath, or a packaged system location.

If the host should discover ScheduleIO without linking its C++ ABI, use the exported C factory entry
points described in [Dynamic Loading](DynamicLoading.md). The created `MppIO` or `XmlIO` object must
always be destroyed by the matching library function.

## Common configuration problems

| Symptom | Likely cause | Fix |
| :--- | :--- | :--- |
| CMake cannot find `Qt6Config.cmake` | Qt's prefix is not on CMake's search path | Set `CMAKE_PREFIX_PATH` to the selected Qt kit. |
| CMake asks for Qt Test | Tests are enabled | Install Qt Test or configure with `-DSCHEDULEIO_BUILD_TESTS=OFF`. |
| Windows link errors mention incompatible runtimes | Qt and the application use different compilers/architectures | Select one matching Qt kit and rebuild all components. |
| The executable starts but cannot load ScheduleIO | The shared library or Qt Core is absent from the loader path | Deploy the run-time libraries beside/binside the application or configure rpaths. |
| A direct `cmake --install` workflow produces no package | Install/package rules are not implemented yet | Integrate with `add_subdirectory()` or `FetchContent` for now. |
