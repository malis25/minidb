# Suggested Learning Roadmap

This roadmap suggests small, testable directions for growing MiniDB. It is not
a commitment that every item will be implemented.

## 1. Complete the object API

Add field-level access, batch operations, pagination for ID and query results,
API versioning, compound/range queries, and consistent error codes. Keep
clients independent of implementation details and add integration tests for
every endpoint and status.

## 2. Improve segmented storage

`ObjectStorage` separates object behavior from persistence;
`SegmentedStorage` currently manages one manifest and multiple bounded
append-only files. Future work includes crash-safe compaction, rotation policy,
cross-process locking, and broader recovery tests before using it for larger
databases.

## 3. Improve record integrity and migrations

Add checksums and an explicit format migration policy. Test interrupted writes,
corrupted records, and supported older versions without relying on
timing-sensitive tests.

## 4. Define multi-key operations

Design atomic batches or transactions before implementing them. Specify what
clients observe when any part of a batch fails and test both memory and disk
state.

## 5. Harden the HTTP service

Before any non-local deployment, design authentication, authorization,
transport security, request timeouts, rate limits, and resource limits. Keep
loopback-only binding as the safe default.

## 6. Measure before optimizing

Add reproducible benchmarks for expected workloads before replacing the
container or storage format. Use measurements to guide indexing, caching, or
more advanced storage structures.

## 7. Consider broader tooling

As the project grows, consider CI builds on multiple compilers, sanitizers,
format checks, fuzz tests for the binary reader, and documented release and
compatibility policies.
