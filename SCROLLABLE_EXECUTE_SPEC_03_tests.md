# Spec — Commit 3/3: Tests (scrollable Execute)

Part of the scrollable-Execute feature. See `SCROLLABLE_EXECUTE_DESIGN.md` for
rationale and `SCROLLABLE_EXECUTE_SPEC.md` for the index. Targets master.
**Depends on commits 1 (backend) and 2 (libpq)** — the test module calls
`PQsendExecutePortal`.

**Scope of this commit:** extend the existing
`src/test/modules/libpq_protocol_cursor/` module (added by the v4 Bind patch).
Do **not** create a new module.

---

## Where it plugs in

The v4 patch already provides:
- `src/test/modules/libpq_protocol_cursor/libpq_protocol_cursor.c` — a C program
  with one function per test, a `print_test_list()`, and a `main()` dispatch.
- `t/001_libpq_protocol_cursor.pl` — TAP driver that runs every name from
  `<binary> tests`, connecting with
  `protocol_cursor=1 max_protocol_version=latest` (except the
  `cursor_options_without_extension` case, which uses `protocol_cursor=0`).

For each new test below: add a `test_<name>(PGconn *)` function, a
`printf("<name>\n")` in `print_test_list()`, and an `else if` in `main()`. The
`.pl` harness picks them up automatically. Follow the existing helpers
(`confirm_result_status`, `consume_result_status`, `consume_null_result`,
`pg_fatal`).

## Test matrix

| # | Test name | Setup | Action | Expect |
|---|-----------|-------|--------|--------|
| 1 | `exec_forward` | SCROLL portal, 5 rows | `PQsendExecutePortal(FWD, 2)` | 2 rows, ids 1-2; tag count 2 |
| 2 | `exec_backward` | SCROLL portal, fwd 3 first | `PQsendExecutePortal(BACK, 1)` | 1 row, id 2 |
| 3 | `exec_absolute` | SCROLL portal | `PQsendExecutePortal(ABS, 4)` | 1 row, id 4 |
| 4 | `exec_relative` | SCROLL portal at row 2 | `PQsendExecutePortal(REL, 2)` | 1 row, id 4 |
| 5 | `exec_short_count` | SCROLL portal, 3 rows, at row 2 | `PQsendExecutePortal(FWD, 100)` | remaining rows only; tag = actual count |
| 6 | `exec_backward_no_scroll` | NO_SCROLL portal | `PQsendExecutePortal(BACK, 1)` | error "cursor can only scan forward" (`PGRES_FATAL_ERROR`) |
| 7 | `exec_wrong_strategy` | portal over a non-fetchable strategy | `PQsendExecutePortal(BACK, 1)` | `ERRCODE_FEATURE_NOT_SUPPORTED` |
| 8 | `exec_no_extension` | conn without extension (`protocol_cursor=0`) | `PQsendExecutePortal(...)` | API returns 0 (client-side, no message sent) |
| 9 | `exec_bad_direction` | raw crafted Execute, direction = 99 | crafted message | `ERRCODE_PROTOCOL_VIOLATION` |
| 10 | `exec_holdable_fwd_across_commit` | HOLD portal, COMMIT, then exec | `PQsendExecutePortal(FWD, PQ_FETCH_ALL)` | all rows after commit |
| 11 | `exec_holdable_back_across_commit` | HOLD+SCROLL portal, COMMIT, fwd then back | `PQsendExecutePortal(BACK, 1)` | correct prior row after commit (**priority case**) |
| 12 | `exec_scrollable_one_call` | prepared SELECT, 5 rows | `PQsendQueryScrollable(SCROLL, FWD, 2)`, then `PQsendExecutePortal(FWD, 2)` | 2 rows ids 1-2 from a **single** result, then ids 3-4; proves the one-command Bind+Describe+Execute path |

Assertions:
- Tests 1-5, 10, 11 assert both **row values** (`PQgetvalue`) and the
  **tag/row count**. Use a SCROLL or HOLD+SCROLL portal created via
  `PQsendBindWithCursorOptions` (from the v4 API) so the portal is actually
  scrollable, then drive it with `PQsendExecutePortal`.
- Test 5 specifically asserts the short-count CommandComplete reports the
  **actual** rows returned, not the requested 100, and that the verb is the
  query's (e.g. `SELECT`), not `FETCH` — guards the commit-1 B5 decision.
- Test 8 must run under `protocol_cursor=0`; add its name to the `.pl` harness's
  special-casing list alongside `cursor_options_without_extension`.  It must
  also assert that `PQsendQueryScrollable` is refused client-side, since that
  function requires the extension even with `cursorOptions == 0`.
- `PQsendExecutePortal` and `PQsendQueryScrollable` each send a
  `Describe Portal` ahead of the Execute (commit 2), so every one of these
  tests receives exactly one `PGRES_TUPLES_OK` result per call, whose
  `PQnfields` is set from that Describe.  A test that expects a bare
  `PGRES_COMMAND_OK` describe result in between would be wrong.

## Test 9 (bad direction) — implementation note

No libpq API path produces an out-of-range direction (`PQsendExecutePortal`
validates client-side per commit-2 step 4). Options:
- (a) Add a small raw-message helper in the C module that hand-builds an Execute
  with direction = 99 and writes it via the existing connection's socket, then
  asserts `PGRES_FATAL_ERROR` + SQLSTATE `08P01`
  (`ERRCODE_PROTOCOL_VIOLATION`); **or**
- (b) skip the automated test and `log()` (in the commit message / test comment)
  that the bad-direction wire path is covered only by backend code review, not
  an automated test.

Pick (a) if the raw-socket helper is cheap to add to the module; otherwise (b).
Do not silently omit — state which was chosen.

## Holdable-across-commit (tests 10, 11) — the priority cases

These exercise the scenario the whole feature is justified by: a `WITH HOLD`
portal materialized at commit, then driven scrollably *after* the transaction
ends. Structure (mirrors the v4 `test_holdable_cursor` flow):

1. `BEGIN`; create temp table + insert known rows; `PQprepare`.
2. Enter pipeline mode; `PQsendBindWithCursorOptions(... PQ_BIND_CURSOR_HOLD
   [| PQ_BIND_CURSOR_SCROLL])`.
3. `COMMIT` (materializes the portal).
4. `PQsendExecutePortal(... FWD ...)` then, for test 11, `PQsendExecutePortal(...
   BACK ...)`.
5. `PQsendClosePortal`; `PQpipelineSync`; drain and assert results.

Test 11 asserts the backward fetch after commit returns the correct prior row —
this is the path that DECLARE/FETCH forces a textual round-trip for today.

---

## Build / verify (this commit)

- `meson test -C build libpq_protocol_cursor` (TAP) — all names green.
- Or run a single case directly:
  `libpq_protocol_cursor exec_holdable_back_across_commit \
     "<conninfo> protocol_cursor=1 max_protocol_version=latest"`.
- The module is registered in `src/test/modules/meson.build` and
  `src/test/modules/Makefile` by the v4 patch — no new registration needed.

## Definition of done

- All 11 (or 10, if test 9 is path (b)) cases pass under `meson test`.
- The full feature is now exercised end-to-end: Bind-with-cursor-options
  (v4) + scrollable Execute (commits 1-2) over forward/backward/absolute/
  relative, including the holdable-across-commit priority cases.
