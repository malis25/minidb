# Metadata and Segment File Format

The database uses one metadata manifest and a sequence of bounded
append-only data segments. The database name is a filename prefix, not a
database file itself.

For `--db-dir data --db-file users`, the storage creates:

```text
data/
  users.manifest.json
  users.segment-00000000.minidb
  users.segment-00000001.minidb
  ...
```

The manifest is the catalog: it records the format version, configured maximum
segment size, next numeric object ID, and ordered segment filenames. Segment
names are generated and validated in sequence; the manifest cannot select an
arbitrary path. A missing manifest while segment files remain is an error, not
an instruction to overwrite or silently reinitialize data.

## Manifest

The manifest is UTF-8 JSON written to a temporary sibling and atomically
replaced. Its logical shape is:

```json
{
  "format": "MINIDB-MANIFEST-1",
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
| Operation | One byte: `S` (store/replace) or `E` (erase) |
| Object ID | Unsigned 64-bit little-endian database ID |
| Object byte length | Unsigned 64-bit little-endian length; `S` records only |
| Object | UTF-8 JSON object bytes; `S` records only |

Records are replayed in segment order and file order. A later store replaces
the same ID; an erase removes it. All data segments have the same maximum byte
limit, including their eight-byte signature. A record is never split between
files. If it will not fit in the active segment, a new segment is created and
added to the manifest before the record is appended. An object too large to
fit in an empty segment is rejected.

## Recovery and validation

Startup validates the manifest, segment sequence, signatures, record sizes,
operations, IDs, and JSON objects. A complete malformed record causes startup
to fail. An incomplete record at the end of the final segment is truncated to
the start of that record; earlier complete records remain available. An
incomplete record in an earlier segment is an error. A manifest-listed final
segment that is missing is recreated empty, which handles a crash after
recording the new segment in the manifest but before creating its file. A
missing earlier segment is an error.

If the process stops after appending a complete new-object record but before
persisting the advanced `next_id`, recovery finds that ID while scanning and
advances the counter. If the process stops before a complete append, the
incomplete bytes are removed and the ID may safely be used on the next create.

## Limits and migration

The previous `MINIOBJ1` single-file prototype and `MINIDB1` key/value snapshot
formats are not compatible with this segmented format. Keep a backup before
using a path that contains data from an older version. The manifest and
segments currently have no checksum or migration tool.

Segments are append-only and are not compacted. Updates and deletes therefore
continue to consume space, and removed values remain in older records until a
future compaction feature is implemented. All live objects and their indexes
are loaded into memory. Only one process may use a database directory at a
time; there is no inter-process lock.

Each record is flushed before success is reported, but this is not a guarantee
against all power-loss or storage-device failures.
