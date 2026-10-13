# HTTP API

MiniDB exposes a JSON-over-HTTP API. A server manages multiple named
databases. Every object operation includes the database name in its path, so
IDs, indexes, storage files, and object contents are isolated per database.

## Start the server

From PowerShell at the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\minidb.exe --db-dir data
```

The server listens on `127.0.0.1:8080` by default. `--db-dir` is the root
directory for all databases. Existing active databases under this root are
loaded on startup. Each database's object schema and maximum objects per
segment are stored in its manifest.

State-changing requests with an `Origin` header are accepted only when the
origin exactly matches the request host and uses the loopback hostname
`localhost` or address `127.0.0.1`. Cross-origin browser writes are rejected
with HTTP 403. Non-browser clients may omit `Origin`. This is CSRF protection,
not authentication: any local process that can connect to the loopback
listener can still use the API.

Every request with a JSON body must send `Content-Type: application/json`
(optional parameters such as `charset=utf-8` are accepted). JSON nesting is
limited to 128 levels; malformed or deeper JSON returns HTTP 400.

### Command-line options

- `--port PORT`: local port from 1 to 65535 (default `8080`).
- `--db-dir DIRECTORY`: **required** root directory for all database folders.
- `--log-requests`: log request method, path, and response status.
- `--log-details`: also log request and response bodies; bodies can contain
  private object values.
- `--help`: show usage.

The server command line does not set segment limits. Each database creation
request provides `maximum_objects_per_segment`; each segment header stores the
number of newly created objects assigned to that segment. Payload size does not
trigger rollover, and one record is never split between segments.

## Database lifecycle

Database names must start with a lowercase letter and contain only lowercase
letters, digits, hyphens, or underscores. They are limited to 63 characters.
Windows device names such as `con`, `nul`, `com1`, and `lpt1` are reserved and
cannot be used as database names.
Each database has its own directory, ID sequence, manifest, segments, and
indexes.

### `GET /databases`

Lists active database names in alphabetical order:

```json
{"count":2,"databases":["messages","users"]}
```

Use `GET /databases?state=archived` to list archived database names. The
`state` query parameter accepts `active` (the default) or `archived`.

### `POST /databases`

Creates an empty database. Returns HTTP 201:

```http
POST /databases
Content-Type: application/json

{
  "name":"users",
  "maximum_objects_per_segment":10000,
  "schema":{
    "type":"object",
    "properties":{
      "username":{"type":"string"},
      "email":{"type":"string"},
      "friends":{"type":"array","items":{"type":"integer"}}
    },
    "required":["username","email"]
  }
}
```

Response:

```json
{"created":true,"name":"users"}
```

The schema supports `string`, `integer`, `number`, `boolean`, `null`, `object`,
and `array` types; nested objects use `properties` and `required`, and arrays
use an `items` schema. Unknown properties are rejected at every object level.
`maximum_objects_per_segment` must be a positive integer. Invalid names,
schemas, or limits return HTTP 400. An already active or archived name returns
HTTP 409; restore an archived database rather than creating over it.

### `GET /databases/{name}/schema` and `PUT /databases/{name}/schema`

GET returns the persisted schema and segment object-count limit. PUT replaces
the schema only when every existing object satisfies it; otherwise it returns
HTTP 409 and keeps the previous schema. New and replacement objects are
validated against the current schema before any data is written.

### `GET /databases/{name}/info`

Returns the active database name, live object count, and next ID:

```json
{"name":"users","object_count":3,"next_id":4}
```

Unknown database names return HTTP 404.

### `GET /databases/{name}`

Lists the database's object IDs in ascending order. Listing is paginated with
query parameters `limit` and `offset`; defaults are 100 and 0, and the maximum
limit is 1000.

```http
GET /databases/users?limit=2&offset=1
```

```json
{"count":2,"has_more":true,"ids":[1,2],"limit":2,"next_after_id":2,"offset":1,"total":7}
```

`count` is the number returned on this page; `total` is the total number of
live objects. Responses also include `has_more` and, when more rows are
available, `next_after_id`. For a live timeline, prefer cursor pagination:

```http
GET /databases/messages?limit=50&after_id=1234
```

`after_id` is exclusive and returns IDs greater than the cursor in ascending
order. It cannot be combined with `offset`. The cursor is stable as new objects
are appended, unlike offset pagination when earlier rows are deleted. IDs are
the ordering key; applications requiring server-time ordering should store a
sortable timestamp and use a query/index feature that supports it (not currently
provided).

### `DELETE /databases/{name}`

By default, archives the database instead of deleting its files. Its folder is
moved under `<db-dir>/.archive/`, and the database disappears from the active
list. It can later be restored.

To irreversibly delete the database files, explicitly request:

```http
DELETE /databases/users?purge=true
```

If `purge` is omitted or set to `false`, the database is archived. Other values
are rejected with HTTP 400. Successful deletion returns HTTP 200. Unknown
databases return HTTP 404.

### `POST /databases/{name}/restore`

Restores a retained archive to the active database list and reloads its objects
and indexes. It returns HTTP 200 on success, HTTP 404 if no archive exists, and
HTTP 409 if the name is already active.

## Objects, IDs, and queries

Create requests must contain a JSON object. Fields can contain strings,
integers, floating-point numbers, booleans, nulls, arrays, or nested objects.
A JSON field named `"id"` is ordinary application data.

The database assigns each object a sequential unsigned 64-bit ID starting at
zero **within its database**. IDs are monotonic and are not reused after
deletion or restart.

All object endpoints are scoped by database name:

`/databases/{name}` and `/databases/{name}/{id}`. The database root supports
listing IDs with `GET` and creating an object with `POST`; `/info` returns
database metadata, and `/query` searches object properties.

### `POST /databases/{name}`

Creates an object and returns HTTP 201 with its database-generated ID:

```http
POST /databases/users
Content-Type: application/json

{"username":"ada","email":"ada@example.test"}
```

```json
{"id":0,"object":{"email":"ada@example.test","username":"ada"}}
```

Malformed JSON or a non-object root returns HTTP 400. Storage failures return
HTTP 500 and the attempted object is not published in memory.

### `GET /databases/{name}/{id}`

Returns HTTP 200 with the ID and complete object:

```json
{"id":0,"object":{"email":"ada@example.test","username":"ada"}}
```

A malformed or out-of-range ID returns HTTP 400; an unknown database or object
returns HTTP 404.

### `PUT /databases/{name}/{id}`

Replaces the complete object at an existing ID. This is not a partial update
and does not create a missing object. Returns HTTP 200 with the stored object.

```powershell
curl.exe -X PUT http://127.0.0.1:8080/databases/users/0 `
  -H "Content-Type: application/json" `
  --data '{"username":"ada","email":"ada@example.test","friends":[18]}'
```

### `DELETE /databases/{name}/{id}`

Deletes an object and returns HTTP 200:

```json
{"deleted":true,"id":0}
```

Unknown IDs return HTTP 404. A deleted ID is not reused.

### `POST /databases/{name}/query`

Finds objects whose **top-level** field equals a JSON value. Query pagination
uses the same URL parameters as ID listing.

```http
POST /databases/users/query?limit=20&offset=0
Content-Type: application/json

{"field":"email","equals":"ada@example.test"}
```

```json
{
  "count": 2,
  "limit": 20,
  "offset": 0,
  "total": 2,
  "results": [
    {"id":0,"object":{"email":"ada@example.test","username":"ada"}},
    {"id":4,"object":{"email":"ada@example.test","username":"another-ada"}}
  ]
}
```

Every top-level property is indexed in memory. Equality is exact and
type-sensitive. Nested subfields, array-element membership, ranges, text
search, and compound conditions are not supported. Unknown fields or values
return an empty page. Missing or invalid query fields return HTTP 400. Offset
pagination must collect matching IDs to select and order the requested page;
large offset-based result sets therefore require temporary memory proportional
to the number of matches.

Queries support the same exclusive `after_id` cursor as ID listing. The
secondary equality index keeps matching IDs ordered, so cursor reads seek to
the next matching ID and return at most `limit` objects.

### `POST /databases/{name}/batch`

Applies between 1 and 1000 creates, full replacements, and deletes as one
database transaction. Validation happens before persistence; if any ID is
missing, repeated, or any operation is malformed, no operation in the batch is
applied. The response includes IDs allocated to creates, in create-operation
order:

```http
POST /databases/messages/batch
Content-Type: application/json

{"operations":[
  {"type":"create","object":{"conversation_id":17,"sender_id":4,"text":"hello","created_at":"2026-10-13T09:00:00Z"}},
  {"type":"replace","id":12,"object":{"conversation_id":17,"last_message_id":99}},
  {"type":"delete","id":13}
]}
```

```json
{"applied":3,"created_ids":[42]}
```

Supported operation shapes are `{"type":"create","object":{...}}`,
`{"type":"replace","id":N,"object":{...}}`, and
`{"type":"delete","id":N}`. An object ID may appear at most once in a batch.
The batch is persisted as one append-only record, so its complete effect is
replayed after restart or none of it is applied if the trailing record is
incomplete. The entire encoded batch must fit within one configured segment;
oversized batches are rejected rather than split across segments. HTTP JSON
body limits also apply.

## Client examples

Clients first create a database, then include its name for every object request.

### Python

```python
import json
from urllib.request import Request, urlopen

base = "http://127.0.0.1:8080"

def post(path, body):
    request = Request(
        f"{base}{path}",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(request) as response:
        return json.load(response)

post("/databases", {"name": "users"})
user = {
    "username": "ada",
    "email": "ada@example.test",
    "friends": [12, 18],
    "profile": {"active": True, "rating": 4.75},
}
created = post("/databases/users", user)
user_id = created["id"]
matches = post(
    "/databases/users/query?limit=10&offset=0",
    {"field": "email", "equals": "ada@example.test"},
)
print(user_id, matches["results"])
```

### Node.js

```javascript
const base = "http://127.0.0.1:8080";

async function post(path, body) {
  const response = await fetch(`${base}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!response.ok) throw new Error(await response.text());
  return response.json();
}

await post("/databases", { name: "users" });
const created = await post("/databases/users", {
  username: "ada",
  email: "ada@example.test",
});
const matches = await post(
  "/databases/users/query?limit=10&offset=0",
  { field: "email", equals: "ada@example.test" },
);
console.log(created.id, matches.results);
```

Java and C# clients use the same JSON and HTTP contracts. Create or restore a
database first, and include its name in every object endpoint.

## Operational and security limits

Request bodies are limited to 1 MiB and JSON nesting is limited to 128 levels.
Wrong or missing JSON content types return HTTP 415; cross-origin writes return
HTTP 403. Invalid requests generally return a JSON body with an `error` string;
missing resources return HTTP 404, name conflicts return HTTP 409, and storage
failures return HTTP 500. All live objects and equality indexes are loaded into memory. An
operating-system lock prevents two MiniDB server processes from opening the
same database root at once. The listener binds only to loopback and has no
authentication, authorization, or TLS.
