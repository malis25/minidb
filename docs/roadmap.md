# Chat-Workload Readiness

MiniDB can serve as a local persistence component for a chat prototype, but it
is not a complete or production-ready chat database/service.

## Available foundations

- Separate databases can hold users, conversations, membership records, and
  messages, each with independent IDs and storage directories.
- Message objects can be queried by exact top-level values such as
  `conversation_id`; ordered ID cursors support fetching older messages.
- A single database batch can atomically create messages, update a conversation
  summary, and update related objects in that same database.
- Segments rotate when their configured count of newly created objects is
  reached, recover incomplete trailing records, and preserve complete
  operations across restart. The count is not a physical byte-size limit.
- Object and query endpoints are paginated; JSON depth and request size are
  bounded.

## Required before production

1. **Security and tenancy:** authenticated identities, authorization for every
   conversation/member/message, encrypted transport, secret management, audit
   policy, and abuse/rate controls belong in a trusted application service.
   The current server binds to loopback and has no authentication or TLS.
2. **Growth and durability:** implement and fault-test crash-safe compaction,
   checksums, automated consistent backups, restore drills, disk-full handling,
   and migration tools for the older prototype formats. The current append-only
   files grow indefinitely and live objects/indexes all reside in memory.
3. **Scale and availability:** define supported database sizes and workload
   limits; benchmark representative message read/write patterns; add metrics,
   operational alerts, and a tested replication/failover plan if availability
   requires it. Only one process may open a data root.
4. **Chat semantics:** the API does not provide WebSockets, publish/subscribe,
   delivery acknowledgements, unread counters, membership enforcement,
   full-text search, timestamp/range queries, or transactions across separate
   databases. Implement these in an application service or extend MiniDB with
   explicit contracts and tests.
5. **Verification:** add fuzzing for record/JSON parsing, failure-injection
   storage tests, and CI coverage on supported operating systems and compilers.

Avoid claiming production readiness until the deployment-specific security,
durability, performance, recovery, and restore requirements have measurable
acceptance criteria and pass them.
