# Metadata and Segment File Format

The database uses one metadata manifest and a sequence of append-only data
segments. The database name is a filename prefix, not a database file itself.

For `--db-dir data` and the database name `users`, the storage creates:

```text
data/
  users/
    users.manifest.json
    users.segment-00000000.minidb
    users.segment-00000001.minidb
    users.index-0f2a....tree
    ...
  messages/
    messages.manifest.json
    messages.segment-00000000.minidb
    ...
  .archive/
    retired-database/
      retired-database.manifest.json
      ...
```

Each active database gets a dedicated subdirectory under `--db-dir`. The
reserved `.archive` directory contains logically deleted databases whose files
are retained for possible restoration. Database names start with a lowercase
letter and then contain only lowercase letters, digits, hyphens, or
underscores. The manifest is the per-database catalog: it records the format
version, object schema, maximum number of newly created objects per segment,
next numeric object ID, ordered segment filenames, the indexed field names, the
object-cache byte budget, and whether a schema migration is still pending.
Segment names are generated and validated in
sequence; the manifest cannot select an arbitrary path. A missing manifest while
segment files remain is an error, not an instruction to overwrite or silently
reinitialize data.

## Manifest

The manifest is UTF-8 JSON written to a temporary sibling and atomically
replaced. Its logical shape is:

```json
{
  "format": "MINIDB-MANIFEST-5",
  "maximum_objects_per_segment": 10000,
  "schema": {
    "type": "object",
    "properties": {
      "username": {"type": "string"},
      "email": {"type": "string"}
    },
    "required": ["username", "email"]
  },
  "indexed_fields": ["username"],
  "cache_bytes": 67108864,
  "schema_migration_pending": false,
  "next_id": 5,
  "next_id_reservation": 128,
  "segments": [
    "users.segment-00000000.minidb",
    "users.segment-00000001.minidb"
  ]
}
```

`next_id` is the ID to allocate for the next created object. Because IDs are
reserved in blocks rather than persisted on every store, `next_id` is a
checkpoint: it can lag the true next ID until a block is refreshed or the
database is closed. `next_id_reservation` is the exclusive upper bound of IDs
that have already been promised by a manifest write. On reopen the effective
next ID is the larger of the two, so a reserved ID is never handed out twice
even after an unclean stop. The reservation block size is a construction-time
policy (`default_next_id_reservation`, 128) and is not required to match the
stored value.
The schema is enforced for all new and replaced objects. The positive
object-count limit is chosen when creating the database and is thereafter stored
in metadata.

`schema_migration_pending` is `true` while existing objects still have to be
adapted to the stored schema after a schema change. It is written before the new
schema so that the schema and the flag are always consistent: if the process
stops at any point, the next open completes the adaptation for whichever schema
is persisted, then clears the flag. Older manifests that omit the field are read
as `false`.

`indexed_fields` lists the fields that have an on-disk B+ tree index.
`cache_bytes` is the byte budget for the per-database in-memory object cache.
When both are empty/zero the database operates exactly like the original
all-in-memory mode: every
live object is held in memory. The manifest is authoritative on reopen, so a
database reopens in the same storage mode it was last saved with.

## Segment records

Every current segment begins with the eight-byte signature `MINIDB4` followed
by a zero byte. The remaining 48-byte header contains two counter slots; each
slot stores a generation, the number of IDs first created in this segment, and
a checksum. The newest valid slot is used, so an interrupted header update
leaves the preceding slot available. Records follow the 56-byte header until
end-of-file. Each record starts with an unsigned 64-bit little-endian payload
length and ends with an unsigned 64-bit little-endian FNV-1a checksum over the
rest of the record body.

| Payload | Encoding |
|---|---|
| Operation | One byte: `S` (store/replace), `E` (erase), or `T` (atomic batch) |
| Object ID | Unsigned 64-bit little-endian database ID |
| Object byte length | Unsigned 64-bit little-endian length; `S` records only |
| Object | UTF-8 JSON object bytes; `S` records only |
| Checksum | Unsigned 64-bit little-endian FNV-1a over all preceding bytes |

A `T` record uses a reserved zero object ID, followed by an unsigned 64-bit
operation count. The payload then contains that many inline operations: `S`,
object ID, object byte length, and JSON object bytes, or `E` and object ID. The
operation count and payload must match exactly.

Records are replayed in segment order and file order. A later store replaces
the same ID; an erase removes it. A record is never split between files. Each
newly created ID consumes one slot in the current segment; reaching the
configured count creates and registers a new segment before writing the next
new object. Replacements and deletes do not consume this count, so they may
continue to grow the physical segment in bytes. Batches count each newly
created ID and remain a single atomic record.

## Recovery and validation

Startup validates the manifest, segment sequence, signatures, header slots,
record sizes, record checksums, operations, IDs, JSON objects, and schema
conformance. A complete malformed record — including one whose checksum does not
match, which detects an in-place bit flip that leaves the length intact — causes
startup to fail rather than drop the record. An incomplete record at the end of
the final segment is truncated to the start of that record; an incomplete
record in an earlier segment is an error. The segment object count is reconciled
while records are already being replayed. A missing manifest-listed segment is
an error unless it is the sole initial segment and the reconciled next ID is
still zero (both `next_id` and `next_id_reservation` are zero); only that
provably empty initialization state can be safely recreated.

When a new segment is needed, the empty file is created before the manifest is
updated, and the record is appended only after both steps succeed. If a crash
leaves the expected next segment unlisted, startup adopts it only if it has
the exact segment header and contains no records, then persists the corrected
manifest. Unexpected or non-empty unlisted segments are rejected rather than
ignored.

If the process stops after appending a complete new-object record but before
persisting the advanced `next_id`, recovery finds that ID while scanning and
advances the counter. If the process stops before a complete append, incomplete
bytes are removed and the ID may safely be used on the next create.

IDs are reserved in blocks so the manifest does not have to be rewritten on
every create. When the in-memory next ID would cross `next_id_reservation`, the
bound is extended by the reservation block size and the manifest is rewritten
once. After an unclean stop, recovery resumes at the larger of the checkpoint,
the reserved bound, and one past the highest ID found while scanning, so IDs in
the unused tail of a block are skipped rather than reused. A graceful close
collapses the reservation back onto the exact next ID, so a clean restart does
not skip IDs.

## Disk-backed indexes and object cache

A database is disk-backed when it has at least one indexed field or a non-zero
cache budget. In that mode live object bodies are not all held in memory;
`get`, `find`, and `find_after` read object bytes back from segments on demand.

For each indexed field the storage maintains a sorted B+ tree in its own file,
named `<database>.index-<hex>.tree` where `<hex>` is the 16-digit hexadecimal
FNV-1a hash of the field name. The tree maps a 16-byte key to an empty value:
an unsigned 64-bit big-endian FNV-1a hash of the encoded field value followed by
the unsigned 64-bit big-endian object ID. Equality lookup seeks to the 8-byte
hash prefix and scans keys sharing it. Because the key stores a hash rather than
the full value, the object is read back and its field is re-checked to rule out
collisions.

Index trees are rebuilt from the segment log when the database is opened, so
they never need to be repaired or listed in the manifest. Creating an index at
runtime adds a field to the manifest and backfills its tree from existing
records; dropping an index removes the field and deletes the tree file. Every
open also rebuilds the in-memory map from object ID to its segment location;
this map is kept in both storage modes so indexes can be created at any time.

The object cache is a byte-bounded least-recently-used cache of object bodies,
sized by `cache_bytes` (set at database creation or via the server's
`--cache-mb` option). Reads consult the cache first and populate it on a miss;
writes and erases update or evict the affected entry.

## Limits and unsupported formats

Manifest format 5 defines the schema, object-count limit, `MINIDB4` headers, the
indexed field list, the cache budget, and the reserved next-ID bound
(`next_id_reservation`, optional for compatibility with manifests written before
it existed). There is no migration or backward
compatibility: any other manifest format and any segment without the `MINIDB4`
signature are rejected on startup rather than converted.

Segments are append-only and are not compacted. Updates and deletes therefore
continue to consume disk space even though they do not consume the new-object
quota; removed values remain in older records. The object-count limit is not a
physical byte-size ceiling. In the default in-memory mode all live objects and
their indexes are loaded into memory; in disk-backed mode object bodies are read
on demand and only the bounded cache, the location map, and the index trees
reside in memory. Only one process may use a database directory at a time. An
operating-system lock file prevents concurrent server processes from opening
the same database root.

## Durability

Each accepted write is forced to stable storage before success is reported.
By default (`durable = true`) the storage opens the segment after flushing its
stream and calls the platform durability primitive (`FlushFileBuffers` on
Windows, `fsync` elsewhere), so the record cannot be lost by a power loss or
kernel panic. Ordering is enforced so recovery can never observe an inconsistent
state: the record reaches stable storage before the manifest, a new segment
reaches stable storage before the manifest lists it, and the manifest's
temporary file is flushed before its atomic rename, after which the containing
directory is flushed (a no-op on Windows, whose filesystem journals directory
metadata).

Durability can be disabled at construction (`durable = false`). The on-disk
format and write ordering are unchanged; only the fsync calls are skipped, so a
process crash is still safe and cannot corrupt or lose committed records, but
the most recent acknowledged writes may be lost if the machine loses power. This
trades the per-write disk barrier for throughput.

Each record is flushed before success is reported, but this is not a guarantee
against all power-loss or storage-device failures. The current object API is
scoped by database name, so each database has an independent ID sequence and
segment list.
