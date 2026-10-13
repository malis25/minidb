# Architecture

## Current scope

MiniDB stores JSON objects in a local database and exposes them through HTTP.
The database assigns sequential `uint64_t` IDs starting at zero. Its in-memory
indexes support lookup by ID and exact equality lookup on each top-level
property. It supports atomic single-database batches. It has no SQL, schema
system, field-path queries, range queries, or cross-database transactions.

## Project layers

```text
HTTP clients
     |
     v
src/server/database_api.cpp ---> DatabaseCatalog
     ^                                   |
     |                                   |                  v
src/server/server.cpp           per-database Database ---> ObjectStorage
     ^                                  |                       |
     +--- CLI and request logger        v                       v
                                 ID/property indexes    SegmentedStorage
                                                           |        |
                                                           v        v
                                                       manifest   bounded
                                                       and files  segments
```

- `include/minidb/object.h` defines a JSON-backed `minidb::Object`.
- `include/minidb/database.h` exposes generated numeric IDs, immutable reads,
  create/replace/delete, atomic batches, cursor and offset pagination, and
  top-level equality queries.
- `include/minidb/storage.h` defines both the storage interface and the
  current `SegmentedStorage` implementation.
- `src/server/database_catalog.cpp` discovers active and archived databases,
  owns their lifecycle, and assigns each a separate directory and lock.
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

The secondary index is `field name -> serialized JSON value -> ordered set of
IDs`. The index is maintained for every top-level field on create, replacement,
and delete. Exact equality queries retrieve the matching ID set in average
O(1); cursor pages seek in O(log k) and read up to the requested number of
matches. The primary ID map also has an ordered ID set for efficient cursor
listing. Updating or deleting one object revises only that object's field
entries. Work and memory grow with the number and size of indexed fields.

Field names nested inside another object are not separate index keys in this
version. Array and nested-object values can be indexed and compared as complete
JSON values, but array element membership is not indexed.

The HTTP adapter serializes operations within a database instance because
cpp-httplib can process requests on several threads. Each database has its own
lock, so independent databases can process requests concurrently. Catalog
changes use a separate lock and wait for in-flight operations on the affected
database. The core class itself is not internally synchronized.

## Manifest and segment responsibilities

`DatabaseCatalog` stores each active database under
`<root>/<name>/<name>.manifest.json` with ordered
`<name>.segment-NNNNNNNN.minidb` data files. Archived databases move to
`<root>/.archive/<name>/` and are not opened until restored. The manifest
contains the segment list, fixed configured size limit, and next ID. Data
files are append-only; each record stores an operation, numeric ID, and (for
stores) a JSON object. A batch is one log record containing all of its
operations, so recovery applies the whole batch or truncates an incomplete
trailing batch. Segment rollover creates a fresh bounded file and records it
in the manifest before appending data.

`Database` depends on the `ObjectStorage` contract rather than parsing file
names or binary records. A future sharded directory backend or another storage
implementation can preserve the object/API contract. The current backend
requires the same max-size setting at restart, loads all live objects into
memory, and has no compaction. An operating-system lock prevents multiple
processes from opening one database root. Pagination limits HTTP response
sizes. Offset pages remain available; ID cursors are preferable for long-lived
message timelines.

## Boundaries and security

The server binds only to `127.0.0.1`; it has no authentication, authorization,
or TLS. Cross-origin browser writes with an `Origin` header are rejected and
JSON request bodies are limited to 1 MiB and 128 nesting levels. Do not expose
it to a network without adding security controls. Use one process at a time per
database root. This is a persistence/API component, not a complete chat
backend: it has no WebSocket delivery, identity/authentication, conversation
membership enforcement, rate limits, replication, backup automation, or
storage compaction. Chat applications must enforce authorization in a trusted
service and must plan backups and growth management before production use.
