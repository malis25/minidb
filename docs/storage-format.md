# Metadata and Segment File Format

The database uses one metadata manifest and a sequence of bounded
append-only data segments. The database name is a filename prefix, not a
database file itself.

For `--db-dir data` and the database name `users`, the storage creates:

```text
data/
  users/
    users.manifest.json
    users.segment-00000000.minidb
    users.segment-00000001.minidb
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
version, configured maximum segment size, next numeric object ID, and ordered
segment filenames. Segment names are generated and validated in sequence; the
manifest cannot select an arbitrary path. A missing manifest while segment
files remain is an error, not an instruction to overwrite or silently
reinitialize data.

## Manifest

The manifest is UTF-8 JSON written to a temporary sibling and atomically
replaced. Its logical shape is:

```json
{
  "format": "MINIDB-MANIFEST-2",
  "maximum_segment_size": 67108864,
  "next_id": 5,
  "segments": [
    "users.segment-00000000.minidb",
    "users.segment-00000001.minidb"
  ]
}
```

`next_id` is the ID to allocate for the next created object. It advances only
after the object record has been appended. During recovery it is reconciled
with IDs found in complete records, so a complete append followed by a crash
before manifest replacement cannot cause an ID to be reused. If the configured
segment limit differs from the manifest, startup fails; reopen with the
original limit.

## Segment records

Every segment begins with the eight-byte signature `MINIDB2` followed by a
zero byte. Records follow until end-of-file. Each record starts with an
unsigned 64-bit little-endian payload length.

| Payload | Encoding |
|---|---|
| Operation | One byte: `S` (store/replace), `E` (erase), or `T` (atomic batch) |
| Object ID | Unsigned 64-bit little-endian database ID |
| Object byte length | Unsigned 64-bit little-endian length; `S` records only |
| Object | UTF-8 JSON object bytes; `S` records only |

`T` records are enabled by manifest format 2 and use a reserved zero object
ID, followed by an unsigned 64-bit operation count. The payload then contains
that many inline operations: `S`, object ID, object byte length, and JSON
object bytes, or `E` and object ID. The operation count and payload must match
exactly.

Records are replayed in segment order and file order. A later store replaces
the same ID; an erase removes it. All data segments have the same maximum byte
limit, including their eight-byte signature. A record is never split between
files. A batch is one record to keep its changes atomic and must fit in a single
segment. If a record will not fit in the active segment, a new empty segment
file is created, added to the manifest, and only then used for the record. An
object too large to fit in an empty segment is rejected.

## Recovery and validation

Startup validates the manifest, segment sequence, signatures, record sizes,
operations, IDs, and JSON objects. A complete malformed record causes startup
to fail. An incomplete record at the end of the final segment is truncated to
the start of that record; earlier complete records remain available. An
incomplete record in an earlier segment is an error. A missing manifest-listed
segment is an error unless it is the sole initial segment and the manifest
still has `next_id` zero; only that provably empty initialization state can be
safely recreated.

When a new segment is needed, the empty file is created before the manifest is
updated, and the record is appended only after both steps succeed. If a crash
leaves the expected next segment unlisted, startup adopts it only if it has
the exact segment header and contains no records, then persists the corrected
manifest. Unexpected or non-empty unlisted segments are rejected rather than
ignored.

If the process stops after appending a complete new-object record but before
persisting the advanced `next_id`, recovery finds that ID while scanning and
advances the counter. If the process stops before a complete append, the
incomplete bytes are removed and the ID may safely be used on the next create.

## Limits and migration

Manifest format 2 adds the `T` batch record. This version reads format-1
manifests and upgrades them when writing format-2 metadata; it writes the
format-2 manifest before appending a batch, so older binaries reject the
database rather than misreading a batch record. The previous `MINIOBJ1`
single-file prototype and `MINIDB1` key/value snapshot formats are not
compatible with this segmented format. Keep a backup before using data with a
different MiniDB version. The manifest and segments currently have no checksum
or migration tool.

Segments are append-only and are not compacted. Updates and deletes therefore
continue to consume space, and removed values remain in older records until a
future compaction feature is implemented. All live objects and their indexes
are loaded into memory. Only one process may use a database directory at a
time. An operating-system lock file prevents concurrent server processes from
opening the same database root.

Each record is flushed before success is reported, but this is not a guarantee
against all power-loss or storage-device failures. The current object API is
scoped by database name, so each database has an independent ID sequence and
segment list. Moving data from the previous flat layout requires moving each
database's manifest and matching segments into a same-named subdirectory before
starting the new server version.
