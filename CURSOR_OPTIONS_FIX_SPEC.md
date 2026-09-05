# Implementation Spec: Fix `_pq_.protocol_cursor` cursor-option handling — Index

Build-ready spec for the defects found reviewing branch `scrollable_execute`
(commit 96842bb "Add `_pq_.cursor` protocol extension for cursor options" plus
the uncommitted scrollable-Execute work in `postgres.c` / `protocol.sgml`).
Companion to `SCROLLABLE_EXECUTE_DESIGN.md` and
`SCROLLABLE_EXECUTE_SPEC.md`; same house style as the
`SCROLLABLE_EXECUTE_SPEC_0N_*.md` files.

The spec is split into one self-contained file per commit. Each file stands
alone — build and verify it independently, in order.

| Commit | File | Scope |
|--------|------|-------|
| 1/3 | [CURSOR_OPTIONS_FIX_SPEC_01_backend.md](CURSOR_OPTIONS_FIX_SPEC_01_backend.md) | Backend: validate Bind cursor options against the plan and portal strategy; plan SCROLL portals with `CURSOR_OPT_SCROLL`; preserve the `NO_SCROLL` default; `MyProcPort` guards; Execute-trailer wire decoupling; message style |
| 2/3 | [CURSOR_OPTIONS_FIX_SPEC_02_libpq_docs.md](CURSOR_OPTIONS_FIX_SPEC_02_libpq_docs.md) | libpq: `nParams` range check, `SCROLL\|NO_SCROLL` rejection, `freePGconn` leak. Docs: `protocol_cursor` conninfo entry, fetch-count sentinel wording, extension attribution of the Bind bitmap, version/naming drift |
| 3/3 | [CURSOR_OPTIONS_FIX_SPEC_03_tests.md](CURSOR_OPTIONS_FIX_SPEC_03_tests.md) | Tests: reproducers for every finding below, plus a differential check against `DECLARE ... SCROLL CURSOR` |

Dependencies: commit 3 depends on 1 and 2. Commit 1 changes behavior that the
existing test `dml_with_cursor_options` asserts (see §T0 in file 03) — commit 1
must therefore be followed by commit 3 before the tree is green, or the two can
be folded if the series is squashed.

Line numbers are anchors against the current working tree, not literals.

---

## Findings this series fixes

| # | Severity | Where | Defect |
|---|----------|-------|--------|
| F1 | HIGH | `postgres.c:2083` | `CURSOR_OPT_HOLD` accepted on any portal strategy. At commit, `PreCommit_Portals` → `HoldPortal` → `PersistHoldablePortal` dereferences a NULL `queryDesc` (`portalcmds.c:340`, `:381`). Reproduced as `SIGSEGV` in a non-assert build for `INSERT ... RETURNING id`, plain `INSERT`, and `SHOW work_mem`; only Bind + COMMIT are needed, no Execute. |
| F2 | HIGH | `postgres.c:2079` | `CURSOR_OPT_SCROLL` is applied *after* the plan is built, so `standard_planner` never adds the Material node (`planner.c:545-549`) and `ExecSupportsBackwardScan` is never consulted, yet `PortalStart` still passes `EXEC_FLAG_BACKWARD` (`pquery.c:506-507`). Measured wrong results: `SELECT id, count(*) FROM t GROUP BY id` with `FETCH 2` then `FETCH BACKWARD 1` returns `1` where `DECLARE c SCROLL CURSOR` returns `3`; `SELECT 1` returns zero rows. |
| F3 | HIGH | `postgres.c:2075` | `portal->cursorOptions = 0` discards the `CURSOR_OPT_NO_SCROLL` default set by `CreatePortal` (`portalmem.c:217`). A client sending `HOLD` alone silently unlocks backward fetch on a portal whose executor never received `EXEC_FLAG_BACKWARD` and whose hold tuplestore is not random-access (`portalmem.c:358`). |
| F4 | MEDIUM | `postgres.c:2078-2081` | `SCROLL｜NO_SCROLL` together is accepted; `transformDeclareCursorStmt` rejects the same combination (`analyze.c:3369-3375`). The executor is initialized for backward scan but every backward fetch errors (`pquery.c:933`). Unrepresentable in SQL. |
| F5 | MEDIUM | `fe-exec.c:1696` | `PQsendBindWithCursorOptions` omits the `nParams` range check its three siblings perform (`fe-exec.c:1527`, `:1573`, `:1667`), so `nParams > 65535` is truncated by `pqPutInt(nParams, 2, ...)` into a Bind message whose parameter count disagrees with its payload. |
| F6 | LOW | `fe-connect.c:5140` | `conn->protocol_cursor` is allocated by conninfo parsing but never released in `freePGconn`. |
| F7 | LOW | `postgres.c:2054`, `:5147` | `MyProcPort` dereferenced unguarded, unlike `:3030` and `:4573`. |

Additional cleanups carried by the series (not defects, but flagged in review):
Execute-trailer direction values are hardcoded to the backend `FetchDirection`
enum; the `PG_INT64_MAX` "fetch all" sentinel is documented as unconditional but
is a literal offset for `ABSOLUTE`/`RELATIVE`; the Bind `Int32 (optional)` field
is documented as belonging to no particular extension; the protocol-extension
table claims "PostgreSQL 19 and later" against a 20devel tree; the commit
message names `_pq_.cursor_bind` / `cursor_protocol` where the code says
`_pq_.protocol_cursor` / `protocol_cursor`; the internal `B4`/`B5`/`B7` spec
tags are still present in code comments.

---

## Normative rule set (shared across commits)

Bind-time cursor options are a *planner and executor* input, not a portal
annotation. The extension must therefore hold to the same contract the SQL
`DECLARE` path holds to. The following rules are normative; commit 1 implements
them, commit 3 tests them one by one.

1. **Flag decoding is additive.** Requested flags are OR-ed into
   `portal->cursorOptions`. `CURSOR_OPT_NO_SCROLL` (set by `CreatePortal`) is
   cleared only when `PQ_BIND_CURSOR_SCROLL` is explicitly requested. Sending
   flags `0` leaves the portal exactly as `CreatePortal` left it. (F3)
2. **`SCROLL` and `NO_SCROLL` are mutually exclusive**, server- and client-side,
   matching `analyze.c:3369`. (F4)
3. **`SCROLL` reaches the planner.** When `SCROLL` is requested, the plan used
   for the portal is built with `CURSOR_OPT_SCROLL` in `cursorOptions`, so the
   planner adds a Material node when the top plan is not backward-scannable.
   Such a plan is never entered into the plan cache as a generic plan. (F2)
4. **`SCROLL` and `HOLD` require `PORTAL_ONE_SELECT`.** Any other strategy is an
   error, not a silently ignored option. This is the only strategy for which
   `PortalStart` builds a `QueryDesc` (`pquery.c:492`) and the only one
   `PersistHoldablePortal` supports (`portalcmds.c:340`); it is also the only
   strategy the SQL path can produce (`portalcmds.c:159`). (F1)
5. **Row-locking clauses are rejected** with `SCROLL` or `HOLD`, reusing the
   existing errdetail strings from `analyze.c:3403-3424`
   ("Scrollable cursors must be READ ONLY." / "Holdable cursors must be READ
   ONLY.").
6. **Wire values are independent of server internals.** The Bind flag bitmap
   already is; the Execute trailer's direction field must be too — decoded
   through an explicit mapping rather than cast onto `FetchDirection`.

> Rule 4 is deliberately stricter than what `PortalRunFetch` can technically
> service (`pquery.c:1407-1434` also handles `PORTAL_ONE_RETURNING`,
> `PORTAL_ONE_MOD_WITH` and `PORTAL_UTIL_SELECT` off the hold tuplestore).
> Those strategies materialize on first Execute and would need their own
> `randomAccess` reasoning and their own hold path; nothing upstream exercises
> them for scroll or hold. Widening rule 4 later is a compatible change,
> narrowing it is not.

---

## Alternatives considered for F2

- **Reject instead of replan.** Error out when the already-built plan fails
  `ExecSupportsBackwardScan` (`executor.h:110`). Smallest patch, but it makes
  `SCROLL` fail for ordinary aggregate, `DISTINCT`, `LIMIT`-less sort-free and
  `SELECT 1` plans — i.e. for most of what a JDBC-style client would ask to
  scroll — while `DECLARE ... SCROLL CURSOR` accepts them all. Rejected as a
  functional regression against the feature's own goal.
- **Mutate `psrc->cursor_options` before `GetCachedPlan`.** Two lines, and
  wrong: `cursor_options` is per-`CachedPlanSource` and is what generic plans
  are built and cached against (`plancache.c:491`, `:1101`). A generic plan
  built without `SCROLL` would be reused for a later `SCROLL` Bind, and one
  built with `SCROLL` would carry a Material node into every non-scroll Bind of
  that statement. Plan-cache revalidation does not consider `cursor_options`.
- **Materialize the portal instead of the plan** (force a hold tuplestore at
  Bind, as `HoldPortal` does at commit). Correct but eagerly runs the query to
  completion at Bind time, destroying streaming for exactly the clients this
  feature targets.
- **Chosen:** a `GetCachedPlan` variant that accepts extra cursor options and
  forces an uncached custom plan when they are non-zero (file 01, §B3). Cost is
  one replan per scrollable Bind, which is what `DECLARE ... SCROLL CURSOR`
  already pays.

---

## G. Patch decomposition

1. Backend + `plancache` API (file 01).
2. libpq + both SGML files (file 02).
3. Tests (file 03).
