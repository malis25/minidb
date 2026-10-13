# HTTP API

MiniDB serves JSON objects over HTTP. It generates a sequential unsigned
64-bit database ID on creation; clients use that numeric ID to read, replace,
and delete the record. A JSON property named `id` is unrelated application
data and is never used as the database key.

## Start the server

From PowerShell at the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\minidb.exe --db-dir data --db-file users --max-file-size 67108864
```

The server listens on `127.0.0.1:8080` by default. The directory, database
name, and maximum segment size are required. A missing database manifest and
first segment are created. Existing databases must be opened with the same
segment-size limit that was used when their manifest was created.

### Command-line options

- `--port PORT`: local port from 1 to 65535 (default `8080`).
- `--db-dir DIRECTORY`: **required** directory for the manifest and segments.
- `--db-file NAME`: **required** database name prefix, not a path.
- `--max-file-size BYTES`: **required** maximum size of each segment file.
- `--log-requests`: log request method, path, and response status.
- `--log-details`: also log request and response bodies; bodies can contain
  private object values.
- `--help`: show usage.

Each segment contains a small header, so the minimum accepted limit is 35
bytes. An individual record that cannot fit in an empty segment is rejected;
records are not split across files. The segment limit includes the file header.
The server binds its port before announcing readiness.

## Object shape and IDs

Create requests must contain a JSON object. Fields can contain strings,
integers, floating-point numbers, booleans, nulls, arrays, or nested objects.
The root must be an object, not a scalar or array.

`POST /objects` does not take an ID. The database assigns the next unsigned
64-bit ID beginning at `0`. The response contains that ID. IDs are monotonic
and are not reused after deletion or restart. Keep the returned ID for later
operations. A JSON field named `"id"` is ordinary object data.

## Endpoints

### `GET /health`

Returns HTTP 200:

```json
{"status":"ok"}
```

### `GET /objects`

Returns live numeric database IDs in ascending order:

```json
{"count":2,"ids":[0,2]}
```

### `POST /objects`

Creates an object under a generated ID. Returns HTTP 201:

```http
POST /objects
Content-Type: application/json

{"username":"ada","email":"ada@example.test","friends":[12,18]}
```

Response:

```json
{"id":0,"object":{"email":"ada@example.test","friends":[12,18],"username":"ada"}}
```

Malformed JSON or a root value that is not an object returns HTTP 400. Storage
failures return HTTP 500 and the attempted object is not published in memory.

### `GET /objects/{id}`

Returns the numeric ID and full JSON object with HTTP 200:

```json
{"id":0,"object":{"email":"ada@example.test","username":"ada"}}
```

A malformed or out-of-range ID returns HTTP 400; an unknown ID returns HTTP
404.

### `PUT /objects/{id}`

Replaces the entire object at an existing numeric ID. This is not a partial
field update. Returns HTTP 200 with the stored ID and object. It does not
create a new object when the ID does not exist (HTTP 404).

```powershell
curl.exe -X PUT http://127.0.0.1:8080/objects/0 `
  -H "Content-Type: application/json" `
  --data '{"username":"ada","email":"ada@example.test","friends":[18]}'
```

### `DELETE /objects/{id}`

Deletes an existing object and returns HTTP 200:

```json
{"deleted":true,"id":0}
```

An unknown ID returns HTTP 404. Deleted IDs are not reused.

### `POST /objects/query`

Finds all objects whose **top-level** field equals the given JSON value. The
field and value are both JSON, so strings, numbers, booleans, nulls, arrays,
and objects can be queried without converting their types to text.

Request:

```http
POST /objects/query
Content-Type: application/json

{"field":"email","equals":"ada@example.test"}
```

Response (IDs are sorted):

```json
{
  "count": 2,
  "results": [
    {"id":0,"object":{"email":"ada@example.test","username":"ada"}},
    {"id":4,"object":{"email":"ada@example.test","username":"another-ada"}}
  ]
}
```

Every top-level property is indexed in memory. Equality is exact and
type-sensitive. Nested subfields, array-element membership, ranges, text
search, and compound conditions are not supported. An unknown field or value
returns HTTP 200 with an empty result. Missing/invalid `field` or `equals`
input returns HTTP 400.

## Client examples

### Python

```python
import json
from urllib.request import Request, urlopen

base = "http://127.0.0.1:8080"
user = {
    "username": "ada",
    "email": "ada@example.test",
    "friends": [12, 18],
    "profile": {"active": True, "rating": 4.75},
}
request = Request(
    f"{base}/objects",
    data=json.dumps(user).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urlopen(request) as response:
    created = json.load(response)
    user_id = created["id"]
    print(user_id, created["object"])

query = Request(
    f"{base}/objects/query",
    data=json.dumps({"field": "email", "equals": "ada@example.test"}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urlopen(query) as response:
    print(json.load(response)["results"])
```

### Node.js

```javascript
const base = "http://127.0.0.1:8080";
const user = {
  username: "ada",
  email: "ada@example.test",
  friends: [12, 18],
  profile: { active: true, rating: 4.75 },
};

const createdResponse = await fetch(`${base}/objects`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(user),
});
if (!createdResponse.ok) throw new Error(await createdResponse.text());
const created = await createdResponse.json();

const queryResponse = await fetch(`${base}/objects/query`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ field: "email", equals: "ada@example.test" }),
});
if (!queryResponse.ok) throw new Error(await queryResponse.text());
console.log(created.id, await queryResponse.json());
```

Java and C# clients use the same JSON and HTTP contracts; use `POST /objects`
to create, retain the returned numeric ID, and send a JSON query body to
`POST /objects/query`.

## Operational and security limits

Request bodies are limited to 1 MiB. Results are not paginated in this MVP.
All objects and equality indexes are loaded into memory on startup. A database
directory is intended for one server process at a time; no inter-process lock
is implemented. The listener binds only to loopback and has no authentication,
authorization, or TLS.
