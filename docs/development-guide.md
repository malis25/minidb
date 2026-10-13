# Development Guide

This guide describes how to build, test, debug, and extend MiniDB on Windows
with CMake.

## Prerequisites

- CMake 3.21 or newer
- A C++20-capable compiler and its build tools
- Git and an internet connection for the first configure: CMake fetches
  cpp-httplib and nlohmann/json; GoogleTest is fetched when tests are enabled

## Configure, build, and test

Run these commands in PowerShell from the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Code formatting

The repository uses the checked-in `.clang-format` configuration. Format
changed C++ files before submitting a change:

```powershell
$files = (Get-ChildItem app -Include *.cpp,*.h -File -Recurse | ForEach-Object FullName) +
    (Get-ChildItem include\minidb -Include *.h -File -Recurse | ForEach-Object FullName) +
    (Get-ChildItem src -Include *.cpp,*.h -File -Recurse | ForEach-Object FullName) +
    (Get-ChildItem tests\unit -Include *.cpp,*.h -File -Recurse | ForEach-Object FullName)
clang-format -i $files
```

Use the checked-in `.clang-format` file for both command-line and editor
formatting.

If generated project files become stale after changing targets, configure the
existing `build/` directory again. It is generated output and ignored by Git;
do not put source files or user database files there.

## Run and debug

```powershell
.\build\Debug\minidb.exe --help
.\build\Debug\minidb.exe --version
.\build\Debug\minidb.exe --db-dir data
```

Configure the `Debug` build, then build the active target and run the tests:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

The main application and unit-test executables are separate targets. The
version implementation is application code under `app/`; it is not linked
into `minidb_core`.

## Object model and storage boundary

1. `minidb::Object` is a JSON object. Properties can hold strings, integers,
   floating-point values, booleans, nulls, arrays, or nested objects.
2. `Database::create(object)` assigns sequential `uint64_t` IDs beginning at
   zero. An object's own optional `id` property is ordinary data.
3. The primary hash index looks up IDs in average O(1). A secondary hash index
   supports exact equality on each top-level property. Nested field paths,
   array membership, and range queries are not implemented.
4. Reads return immutable shared snapshots. `ObjectStorage` isolates the core
   from persistence; `SegmentedStorage` writes a manifest plus append-only data
   segments capped by the count of newly created objects.
5. A storage mutation completes before `Database` publishes the new value.
   Persistence errors are surfaced to the caller.

When changing an indexed operation, test index maintenance for create,
replacement, and delete. When changing storage, test rollover, manifest
recovery, and interrupted final records. Update the API and storage guides with
externally visible behavior.

### C++ example

```cpp
minidb::Database database;

const minidb::Object user = {
    {"username", "ada"},
    {"email", "ada@example.test"},
    {"friends", {"user-18", "user-27"}},
    {"profile", {{"active", true}, {"score", 9.5}}},
};

const std::uint64_t user_id = database.create(user);
const auto saved_user = database.get(user_id);
```

The returned `user_id` is the database's generated numeric ID. An `"id"`
property inside `user` is not interpreted as the database ID. `get` returns an
immutable shared object or an empty pointer when the ID is missing. The
default-constructed database above is in-memory only. To persist objects,
construct it with `std::make_unique<minidb::SegmentedStorage>(directory,
database_name, maximum_objects_per_segment, schema)`. The server reads the
database root from `--db-dir`. Each database's schema and
`maximum_objects_per_segment` are set on `POST /databases`.

## Run the HTTP server

```powershell
.\build\Debug\minidb.exe --port 8080 --db-dir data
```

The server provides database lifecycle endpoints under `/databases` and
database-scoped object endpoints directly under `/databases/{name}`. Create a
database using `POST /databases` with its schema and segment object limit
before using its object API. `GET /databases/{name}` lists IDs,
`POST /databases/{name}` creates a schema-validated object,
`GET|PUT /databases/{name}/schema` reads or updates its schema (only when all
existing objects pass validation), and `POST /databases/{name}/batch` atomically
applies related changes in that database. IDs are independent per database.
ID lists and equality queries accept
`limit` and `offset` query parameters (default page size 100, maximum 1000).
Deleting a database archives its files by default; use `?purge=true` only for
permanent removal. The server binds to `127.0.0.1`.

Use a separate temporary data directory when manually exercising the API:

```powershell
.\build\Debug\minidb.exe --port 8080 `
    --db-dir $env:TEMP\minidb-http-data
```

Request logs are opt-in with `--log-requests`; `--log-details` also prints
request and response bodies. Treat detailed output as sensitive. Do not run multiple server processes against the same database directory.

## Write persistence tests safely

Persistence tests must not create files in the source tree. Tests create
unique directories under `std::filesystem::temp_directory_path()` and remove
them after each test. Follow that pattern for tests that read or write files.

User-created databases have separate subdirectories under `data/`; the server
creates the root directory if it does not exist. Archived databases live under
`data/.archive/`. These files are ignored by Git. Never use real user data as
test input.

## Error handling

`SegmentedStorage` and `Database` report invalid paths, malformed records, invalid
objects, and I/O errors with exceptions. A storage error does not publish the
attempted mutation in the in-memory database. An incomplete final record is
recovered by truncating only that incomplete suffix; a malformed complete
record fails startup.
