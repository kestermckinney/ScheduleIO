# Contributing to ScheduleIO

Thanks for helping improve ScheduleIO. Contributions are welcome for file-format compatibility,
scheduling behavior, tests, examples, documentation, and focused API improvements.

## Before opening an issue

Search the existing issues first. A useful report includes:

- ScheduleIO release or commit
- Operating system, compiler, CMake version, and Qt version
- Input format and Microsoft Project version when known
- The operation being performed: read, edit, schedule, MPP save, or XML save
- Expected and actual behavior, including `errorString()` when relevant
- A minimized, non-sensitive `.mpp`/`.xml` fixture when the problem is format-specific

Do not publish schedules containing confidential project, customer, personnel, cost, or calendar
data. Reduce or synthesize a fixture before attaching it.

## Building and testing changes

Follow the [build guide](docs/GettingStarted/Building.md) and run:

```sh
cmake -S . -B build-tests \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSCHEDULEIO_BUILD_TESTS=ON
cmake --build build-tests --config RelWithDebInfo --parallel
ctest --test-dir build-tests -C RelWithDebInfo --output-on-failure
```

Changes to parsing or writing should include a focused unit or round-trip test. When a change relies
on behavior observed in Microsoft Project, add a minimized fixture pair or document the validation
method. Keep generated build output out of commits.

## Pull requests

Please keep each pull request focused. Explain the problem, the chosen behavior, compatibility or
preservation implications, and how the change was tested. Update the documentation when public
fields, units, defaults, I/O coverage, or build requirements change.

ScheduleIO's model uses public Qt value types and integer unique-ID links. Preserve existing values
that the writer does not understand, avoid introducing QObject ownership into the model, and call
out any semantic-equality or ABI consequences of public structure changes.

## Documentation

Documentation lives in `docs/` and is built with MkDocs:

```sh
python -m pip install mkdocs
mkdocs build --strict
```

Keep member tables synchronized with the public headers, state units and sentinel values explicitly,
and link a new page from `mkdocs.yml` so it appears in navigation.

## Collaboration

Keep reviews respectful, specific, and evidence-based. It is fine to open an issue before investing
in a large change so scope and compatibility can be discussed early.
