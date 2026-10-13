# Architecture

## Current scope

MiniDB stores JSON objects in a local database and exposes them through HTTP.
The database assigns sequential `uint64_t` IDs starting at zero. Its in-memory
indexes support lookup by ID and exact equality lookup on each top-level
property. It has no SQL, schema system, field-path queries, range queries,
transactions, or multi-process locking.

## Project layers

```text
HTTP clients
     |
     v
src/server/database_api.cpp ---> minidb::Database ---> ObjectStorage
     ^                                   |                  |
     |                                   |                  v
src/server/server.cpp              ID/property indexes  SegmentedStorage
     ^                                                      |        |
     +--- CLI and request logger                            v        v
                                                     manifest     bounded
                                                       file       segments
```

- `include/minidb/object.h` defines a JSON-backed `minidb::Object`.
- `include/minidb/database.h` exposes generated numeric IDs, immutable reads,
  create/replace/delete, and top-level equality queries.
- `include/minidb/storage.h` defines both the storage interface and the
  current `SegmentedStorage` implementation.
- `src/database.cpp` owns the live object map and derived property indexes.
- `src/segmented_storage.cpp` owns manifest and segment files, record replay,
  file rollover, and persistence.
- `src/server/` maps HTTP requests to database operations and handles
  configuration and logging.
- `app/main.cpp` and `app/version.cpp` are executable-only code. The version
  implementation is not part of `minidb_core`.

## ID and object lifecycle

Create accepts a complete JSON object with no database ID. `Database` uses
`next_id`, initially zero, and returns that `uint64_t` to the caller. The
object's own optional `id` field is just data. Updates and deletes address an
existing database-generated ID; a deleted ID is never reused. The manifest
persists the next ID, and recovery reconciles it with complete segment records.

## In-memory indexes

The primary map is `uint64_t -> shared_ptr<const Object>`. It provides
average O(1) ID access without copying the full JSON object for each read.

The secondary index is `field name -> serialized JSON value -> set of IDs`.
The index is maintained for every top-level field on create, replacement, and
delete. Exact equality queries retrieve their matching ID set in average O(1),
then spend O(k) to assemble k results. Results are sorted by ID, costing
O(k log k). Updating or deleting one object revises only that object's field
entries. Work and memory grow with the number and size of indexed fields.

Field names nested inside another object are not separate index keys in this
version. Array and nested-object values can be indexed and compared as complete
JSON values, but array element membership is not indexed.

The HTTP adapter serializes access to a database instance because cpp-httplib
can process requests on several threads. The core class itself is not
internally synchronized.

## Manifest and segment responsibilities

`SegmentedStorage` persists a database as one `<name>.manifest.json` catalog
and ordered `<name>.segment-NNNNNNNN.minidb` data files. The manifest contains
the segment list, fixed configured size limit, and next ID. Data files are
append-only; each record stores an operation, numeric ID, and (for stores) a
JSON object. Segment rollover appends a new filename to the catalog and starts
writing in a fresh bounded file.

`Database` depends on the `ObjectStorage` contract rather than parsing file
names or binary records. A future sharded directory backend or another storage
implementation can preserve the object/API contract. The current backend
requires the same max-size setting at restart, loads all live objects into
memory, and has no compaction or cross-process coordination.

## Boundaries and security

The server binds only to `127.0.0.1`; it has no authentication, authorization,
or TLS. Request bodies are limited to 1 MiB. Do not expose it to a network
without adding security controls. A single database directory must be used by
only one process at a time.
