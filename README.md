# minidb

MiniDB is a C++20 learning project for storing JSON objects behind a local
HTTP API. The database assigns each object a sequential `uint64_t` ID beginning
at `0`. It maintains an in-memory hash index by ID and equality indexes for
top-level object properties such as `email`, `username`, or `id`. Databases can
optionally index chosen fields with on-disk B+ trees and bound object memory
with a per-database cache, so large datasets need not be held entirely in RAM.

This is an early database MVP. It provides atomic batches within one database,
but not transactions across databases, range queries, field-path indexing,
compaction, or a distributed storage engine. Only one process may open a data
root at a time.

## Build and test

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

CMake fetches cpp-httplib, nlohmann/json, and GoogleTest (GoogleTest only when
`BUILD_TESTING` is enabled).

## Run

```powershell
.\build\Debug\minidb.exe --db-dir data
```

`--db-dir` is required and is the root for all databases. Each database stores
its schema and maximum number of newly created objects per segment in metadata.
Use `--cache-mb MEGABYTES` to set a default per-database object-cache budget for
new databases. Use `--help` for all options or `--version` for the executable
version.

## Databases and generated IDs

Create separate databases for separate collections of objects. Database names
start with a lowercase letter and use lowercase letters, digits, hyphens, or
underscores.

```powershell
curl.exe -X POST http://127.0.0.1:8080/databases `
  -H "Content-Type: application/json" `
  --data '{"name":"users","maximum_objects_per_segment":10000,"schema":{"type":"object","properties":{"id":{"type":"string"},"username":{"type":"string"},"email":{"type":"string"},"friends":{"type":"array","items":{"type":"integer"}}},"required":["username"]}}'
curl.exe -X POST http://127.0.0.1:8080/databases `
  -H "Content-Type: application/json" `
  --data '{"name":"messages","maximum_objects_per_segment":100000,"schema":{"type":"object","properties":{"conversation_id":{"type":"integer"},"sender_id":{"type":"integer"},"text":{"type":"string"},"created_at":{"type":"string"}},"required":["conversation_id","sender_id","text","created_at"]}}'
curl.exe http://127.0.0.1:8080/databases
```

Each database has its own ID sequence, starting at zero, and its own data
directory. IDs in `users` and `messages` are independent.

## Objects and queries

Applications send complete JSON objects. Fields may be strings, integers,
floating-point values, booleans, nulls, arrays, or nested objects. The database
allocates `0`, `1`, `2`, ... and returns the generated ID on create; an `id`
property inside JSON is ordinary user data and is unrelated to that internal
database ID.

```powershell
curl.exe -X POST http://127.0.0.1:8080/databases/users `
  -H "Content-Type: application/json" `
  --data '{"id":"app-user-9","username":"ada","email":"ada@example.test","friends":[12,18]}'
curl.exe http://127.0.0.1:8080/databases/users/0
```

Database object routes use the database path directly:
`GET /databases/{name}` lists IDs, `POST /databases/{name}` creates an object,
`GET|PUT|DELETE /databases/{name}/{id}` operate on one object,
`POST /databases/{name}/query` searches by property,
`GET|PUT /databases/{name}/indexes` reads or replaces the indexed field set, and
`GET /databases/{name}/info` returns database metadata.
`POST /databases/{name}/batch` applies related changes atomically within that
database. Deleting the `/databases/{name}` path archives or purges that
database. Listing IDs and
query results support `limit` and `offset` pagination; use `after_id` cursors
for stable message timeline pages. For example:

```powershell
curl.exe -X POST "http://127.0.0.1:8080/databases/users/query?limit=20&offset=0" `
  -H "Content-Type: application/json" `
  --data '{"field":"email","equals":"ada@example.test"}'
```

Property queries return every match in ascending database-ID order. Equality
preserves JSON types: integer `1`, floating-point `1.0`, string `"1"`, and
boolean `true` are distinct indexed values. See the
[HTTP API guide](docs/http-api.md) for request/response examples in multiple
languages.

Every database has its own object schema. A create or update must satisfy the
schema's required fields and types; undeclared fields are rejected. The schema
can be changed with `PUT /databases/{name}/schema` only if every current object
passes the proposed schema.

## Data files and performance

For database name `users`, the directory contains a metadata file
`data/users/users.manifest.json` and append-only segment files such as
`data/users/users.segment-00000000.minidb`. The manifest records the schema,
objects-per-segment limit, segment list, the next ID, and a reserved next-ID
bound: IDs are persisted in blocks so creating an object does not rewrite the
manifest every time. A segment header stores
the number of objects first created in that segment; reaching the configured
count sends the next newly created object to another segment. Replacements and
deletions do not consume that object count, so they can still increase the
physical bytes in a segment.

Deleting a database without a query option moves it into `data/.archive/`;
`POST /databases/{name}/restore` brings it back. Use
`DELETE /databases/{name}?purge=true` only when you explicitly want to
permanently remove its files.

The core uses hash maps for ID lookup and exact top-level field/value lookup,
both average O(1) relative to the number of indexed keys. Updating an object
updates its field-index entries; storage appends only that object’s record.
Search by an indexed field is average O(1) to locate the matching IDs plus
O(k) to return k objects. Listing all IDs sorts them and takes O(n log n).
Indexing every property uses additional memory and write work proportional to
the number and size of the object's top-level fields.

The `ObjectStorage` boundary separates database behavior from its multi-file
implementation, leaving room for later storage strategies. Compaction is not
implemented, so segments do not shrink after deletes or updates and the data
directory grows over time.

## Project structure and guides

```text
app/                 Executable entry point and version implementation
include/minidb/      Public object, database, and storage interfaces
src/                 Database, secondary indexes, segmented storage, HTTP server
tests/unit/          Core unit and HTTP integration tests
docs/                API, object model, architecture, and format guides
data/                Local database files (ignored by Git)
```

- [Object Model](docs/object-model.md): JSON fields, numeric IDs, and indexes.
- [HTTP API](docs/http-api.md): endpoint contracts and client examples.
- [Architecture](docs/architecture.md): code and storage responsibilities.
- [Development Guide](docs/development-guide.md): build, test, and extension flow.
- [Storage Format](docs/storage-format.md): manifest and segment record layout.
- [Roadmap](docs/roadmap.md): future work.

The server listens only on `127.0.0.1`. Authentication, authorization, and TLS
are not implemented; do not expose it to a network. Cross-origin browser
writes are rejected, and an operating-system lock prevents two server
processes from opening the same data directory simultaneously. See the
[chat-workload readiness roadmap](docs/roadmap.md) for the important limits
before using MiniDB behind a real chat application.

## AI assistance

AI tools were used during the development of this project, including for
implementation, tests, and documentation. The project is not presented as work
completed entirely without AI assistance.
