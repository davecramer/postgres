# Spec — Commit 2/3: libpq (scrollable Execute)

Part of the scrollable-Execute feature. See `SCROLLABLE_EXECUTE_DESIGN.md` for
rationale and `SCROLLABLE_EXECUTE_SPEC.md` for the index. Targets master.
**Depends on commit 1** (backend) for end-to-end behavior. Mirrors the v4 Bind
patch's libpq surface (`PQsendBindWithCursorOptions`, `PQPortalCursorEnabled`,
`PQ_BIND_CURSOR_*`).

**Scope of this commit:** `PQsendExecutePortal`, `PQsendQueryScrollable`, the
`PQ_FETCH_*` constants, the `pqPutInt64` helper, exports.txt, PQtrace decoding
of the new wire fields, and the `libpq.sgml` doc. No backend changes, no tests.

Line numbers are anchors against master, not literals.

---

## C1. `src/interfaces/libpq/libpq-fe.h`

Add near the `PQ_BIND_CURSOR_*` block:

```c
/* Fetch directions for PQsendExecutePortal (_pq_.protocol_cursor extension) */
#define PQ_FETCH_FORWARD    0
#define PQ_FETCH_BACKWARD   1
#define PQ_FETCH_ABSOLUTE   2
#define PQ_FETCH_RELATIVE   3

/* Portable "all rows" count token (maps to FETCH_ALL server-side) */
#define PQ_FETCH_ALL        INT64_MAX
```

`INT64_MAX`, not `PG_INT64_MAX`: the latter lives in `src/include/c.h`, which
client code must not include.  `libpq-fe.h` already includes `<stdint.h>`.

Also add a `LIBPQ_HAS_PROTOCOL_CURSOR` feature macro; the Bind half shipped
without one, and it covers both halves.

Declare:

```c
extern int PQsendExecutePortal(PGconn *conn, const char *portalName,
                             int direction, int64_t count);
extern int PQsendQueryScrollable(PGconn *conn, const char *stmtName,
                                 int nParams, const char *const *paramValues,
                                 const int *paramLengths, const int *paramFormats,
                                 int resultFormat, const char *portalName,
                                 int cursorOptions, int direction, int64_t count);
```

## C2. `src/interfaces/libpq/fe-misc.c` — 64-bit put helper

`pqPutInt` handles only 2/4 bytes (`fe-misc.c:253`). `pg_hton64` is available
frontend-side (`pg_bswap.h` is shared; `fe-encrypt-openssl.c:388` already uses
it), so add a clean helper rather than the split-int32 dance `fe-lobj.c`'s
`lo_hton64` uses for the legacy PQfn path:

```c
int
pqPutInt64(int64_t value, PGconn *conn)
{
    uint64 tmp = pg_hton64((uint64) value);
    return pqPutMsgBytes((const char *) &tmp, 8, conn) ? EOF : 0;
}
```

Declare in `src/interfaces/libpq/libpq-int.h` alongside `pqPutInt` (line ~814).
(Frontend read-back of a 64-bit value is not needed — the server never sends a
count back.)

## C3. `src/interfaces/libpq/fe-exec.c` — entry points

### `PQsendExecutePortal`

Sends a `PqMsg_Execute` whose body is
`portalName, max_rows=0, direction(Int32), count(Int64)`. Pattern follows
`PQsendBindWithCursorOptions`:

1. `PQsendQueryStart(conn, true)`; return 0 on failure.
2. Validate `portalName` non-NULL/non-empty (unnamed portal rejected, as the
   Bind-side does) → `libpq_append_conn_error`, return 0.
3. If `!conn->protocol_cursor_enabled` → `libpq_append_conn_error("...require
   the _pq_.protocol_cursor protocol extension")`, return 0.
4. Validate `direction` in `[PQ_FETCH_FORWARD, PQ_FETCH_RELATIVE]` → error,
   return 0.
5. `pqAllocCmdQueueEntry`.
6. Build a `Describe` (`'P'`, portalName) message, **then** the Execute:
   `pqPutMsgStart(PqMsg_Execute)`, `pqPuts(portalName)`, `pqPutInt(0, 4)`
   (max_rows), `pqPutInt(direction, 4)`, `pqPutInt64(count)`, `pqPutMsgEnd`.
7. If `conn->pipelineStatus == PQ_PIPELINE_OFF`: append a `PqMsg_Sync`.
8. `entry->queryclass = PGQUERY_EXTENDED` (it returns rows); `pqPipelineFlush`;
   `pqAppendCmdQueueEntry`; return 1.
9. `sendFailed:` `pqRecycleCmdQueueEntry`, return 0.

`count` is `int64_t`; clients pass `PQ_FETCH_ALL` for all-rows.  A count of `0`
is *not* "all rows": the server treats it as `FETCH FORWARD 0`, which re-fetches
the current row.

**The Describe in step 6 is not optional.** Each libpq call gets a fresh
`PGcmdQueueEntry` with `conn->result == NULL`, and a DataRow arriving without a
RowDescription in that same entry fails the query with *"server sent data
(\"D\" message) without prior row description (\"T\" message)"*
(`fe-protocol3.c`).  The RowDescription obtained when the portal was created
belongs to an already-consumed entry.  Cost: one extra RowDescription per fetch.

Step 8 must **not** assign `conn->asyncStatus`: `pqAppendCmdQueueEntry` already
sets `PGASYNC_BUSY` when appropriate, and overriding it clobbers pipeline state.
Remove the existing assignment from `PQsendBindWithCursorOptions` too.

### `PQsendQueryScrollable`

Convenience that issues, in one call: Bind-with-cursor-options + Describe (P) +
the extended Execute + Sync. Same validation gates as above (named portal
required, extension required for non-zero cursorOptions / any direction).
Returns 1 on success, 0 on failure.

Factor the shared code into a static `PQsendBindGuts(..., bool withExecute, int
direction, int64_t count)` — the same shape as the existing
`PQsendQueryGuts` — and make `PQsendBindWithCursorOptions` and
`PQsendQueryScrollable` thin wrappers over it.  `withExecute` also selects
`queryclass` (`PGQUERY_EXTENDED` vs `PGQUERY_DESCRIBE`).  Factor the Execute
trailer itself into a static `pqPutScrollExecuteMsg`, shared with
`PQsendExecutePortal`, so the trailer's wire format exists in one place.

Chaining the two public functions instead is **not viable**: it produces two
command-queue entries (two rounds of results), and outside pipeline mode the
second call fails with "another command is already in progress" after the Bind
is already on the wire.

## C4. `src/interfaces/libpq/exports.txt`

```
PQsendExecutePortal       214
PQsendQueryScrollable     215
```

(214/215 follow the v4 patch's 212/213.)

## C5. `src/interfaces/libpq/fe-trace.c` — PQtrace decoding

`pqTraceOutput_Execute` decodes only portal + max_rows and
`pqTraceOutput_Bind` stops before the cursor-options Int32, so a traced message
silently drops the new fields — misleading precisely when tracing this feature.
Add a `pqTraceOutputInt64` helper, pass the message `length` into both
functions (the convention `pqTraceOutput_CopyData` already uses), and decode the
trailing fields only when the bytes are present, so trailer-free messages
(and the existing `libpq_pipeline` trace expectations) are unchanged.

---

## D (this commit). Documentation — `doc/src/sgml/libpq.sgml`

Document `PQsendExecutePortal`, `PQsendQueryScrollable`, the `PQ_FETCH_*`
constants, and `PQ_FETCH_ALL`, mirroring the v4 `PQsendBindWithCursorOptions`
entry. Note the extension-required and named-portal-required rules and that
these functions work in pipeline mode, that a count of `0` re-fetches the
current row, and that the completion tag keeps the query's own verb.  Also
extend the `protocol_cursor` conninfo and `PQPortalCursorEnabled` entries, which
currently name only `PQsendBindWithCursorOptions`.

---

## Build / verify (this commit)

- `meson compile -C build` — must link; `exports.txt` ordinals must be
  monotonic (build fails otherwise).
- ABI: new symbols only, no signature changes to existing ones.
- Functional verification happens in commit 3 (the test module calls these).

## Definition of done

- Builds and links; `exports.txt` accepted.
- `PQsendExecutePortal` returns 0 (no crash) when the extension was not
  negotiated, and when given an unnamed portal or out-of-range direction.
- Doc builds (`meson compile -C build doc/src/sgml/postgres-A4.pdf` or the html
  target) without SGML errors.
