# Implementation Spec: Scrollable Execute (`_pq_.protocol_cursor`) — Index

Build-ready spec for the scrollable-Execute feature. Companion to
`SCROLLABLE_EXECUTE_DESIGN.md` (rationale/RFC). Targets master. Builds on the v4
Bind patch, which already adds the `_pq_.protocol_cursor` negotiation
(`port->protocol_cursor_enabled`, `conn->protocol_cursor_enabled`), the conninfo
option `protocol_cursor`, and the libpq Bind-side API.

The spec is split into one self-contained file per commit (the §G decomposition
below). Each file stands alone — build and verify it independently, in order.

| Commit | File | Scope |
|--------|------|-------|
| 1/3 | [SCROLLABLE_EXECUTE_SPEC_01_backend.md](SCROLLABLE_EXECUTE_SPEC_01_backend.md) | Backend: Execute trailer parse + `exec_execute_message` routing + guards + count helper + tag construction + `protocol.sgml` |
| 2/3 | [SCROLLABLE_EXECUTE_SPEC_02_libpq.md](SCROLLABLE_EXECUTE_SPEC_02_libpq.md) | libpq: `PQsendExecutePortal`, `PQsendQueryScrollable`, `PQ_FETCH_*` constants, `pqPutInt64`, exports.txt, `libpq.sgml` |
| 3/3 | [SCROLLABLE_EXECUTE_SPEC_03_tests.md](SCROLLABLE_EXECUTE_SPEC_03_tests.md) | Tests: extend `src/test/modules/libpq_protocol_cursor/` with the 11-case matrix incl. holdable-across-commit |

Dependencies: commit 2 depends on commit 1 for end-to-end behavior; commit 3
depends on both. Stack on top of the v4 Bind patch (or fold into the same
series as v5).

---

## Wire format (normative, shared across commits)

Extended Execute body, present only when `_pq_.protocol_cursor` negotiated:

```
String  portalName
Int32   max_rows            (legacy; ignored when trailer present)
Int32   direction           (0 FWD, 1 BACK, 2 ABS, 3 REL == FetchDirection)
Int64   count               (signed)
```

- Trailer is all-or-nothing (both direction and count, or neither).
- No extension + any trailing bytes ⇒ `ERRCODE_PROTOCOL_VIOLATION`.
- Extension on + no trailer ⇒ existing forward `PortalRun()` path, unchanged.
- max_rows ignored when trailer present (documented; not an error).
- Direction equals the backend `FetchDirection` enum — 1:1 pass-through.
- Count: `PG_INT64_MAX` ⇒ `FETCH_ALL` (portable "all"); `0` ⇒ zero-row no-op
  (== SQL `FETCH FORWARD 0`); other N ⇒ literal after platform-`long` range
  check. Client constant: `PQ_FETCH_ALL`.

---

## Resolved design points (settled from source 2026-06-30)

These were open questions; all resolved by reading master. Detail lives in the
per-commit files; summarized here so the index is self-contained.

- **Command tag (commit 1, B5):** the fetch path preserves the *query's* verb
  (`SELECT <n>`) via `CopyQueryCompletion(&qc, &portal->qc)` +
  `qc.nprocessed = nprocessed`, mirroring `PortalRun`. It does **not** use
  `CMDTAG_FETCH` (that's SQL-level FETCH in `PerformPortalFetch`,
  `portalcmds.c:216`). Scrollable Execute always completes — never
  PortalSuspended.
- **Count `0` / "all" (commit 1, B4):** thin pass-through to `FetchStmt.howMany`
  with `PG_INT64_MAX` as the reserved portable "ALL" token; no `LONG_MAX`
  sentinel collision.
- **64-bit wire helpers (commits 1 & 2, H3):** backend `pq_getmsgint64` exists;
  frontend gets a new `pqPutInt64` (`pg_hton64` is available frontend-side).

---

## G. Patch decomposition

1. Backend (file 01). protocol.sgml in the same commit.
2. libpq (file 02). libpq.sgml in the same commit.
3. Tests (file 03).
