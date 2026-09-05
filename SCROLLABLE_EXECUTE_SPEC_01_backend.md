# Spec — Commit 1/3: Backend (scrollable Execute)

Part of the scrollable-Execute feature. See `SCROLLABLE_EXECUTE_DESIGN.md` for
rationale and `SCROLLABLE_EXECUTE_SPEC.md` for the index across all three
commits. Targets master. Builds on the v4 Bind patch, which already adds the
`_pq_.protocol_cursor` negotiation (`port->protocol_cursor_enabled`).

**Scope of this commit:** backend message parse + routing + guards + count
helper + tag construction, plus the `protocol.sgml` doc change. No libpq, no
tests (those are commits 2 and 3).

Line numbers are anchors against master, not literals.

---

## A. Wire format (normative)

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
- Direction values equal the backend `FetchDirection` enum
  (`src/include/nodes/parsenodes.h:3527`) — 1:1, pass straight through after a
  range check.

---

## B1. `src/backend/tcop/postgres.c` — message dispatch (~line 4909)

Current `case PqMsg_Execute:` reads:

```c
portal_name = pq_getmsgstring(&input_message);
max_rows = pq_getmsgint(&input_message, 4);
pq_getmsgend(&input_message);
...
exec_execute_message(portal_name, max_rows);
```

Change: parse the optional trailer *before* `pq_getmsgend`, then pass it down.

```c
const char *portal_name;
int         max_rows;
int         fetch_direction = FETCH_FORWARD;
int64       fetch_count = 0;
bool        is_scroll_execute = false;

forbidden_in_wal_sender(firstchar);
SetCurrentStatementStartTimestamp();

portal_name = pq_getmsgstring(&input_message);
max_rows = pq_getmsgint(&input_message, 4);

/* Optional scrollable-execute trailer (_pq_.protocol_cursor). */
if (input_message.cursor < input_message.len)
{
    if (!MyProcPort->protocol_cursor_enabled)
        ereport(ERROR,
                (errcode(ERRCODE_PROTOCOL_VIOLATION),
                 errmsg("Execute message contains trailing data but the "
                        "_pq_.protocol_cursor extension is not enabled")));

    fetch_direction = pq_getmsgint(&input_message, 4);
    fetch_count = pq_getmsgint64(&input_message);   /* signed Int64 */
    is_scroll_execute = true;
}
pq_getmsgend(&input_message);

valgrind_report_error_query(...);   /* unchanged surrounding code */

exec_execute_message(portal_name, max_rows,
                     is_scroll_execute, fetch_direction, fetch_count);
```

Notes:
- `pq_getmsgint64()` (`src/include/libpq/pqformat.h:199`, returns `int64`).
  A partial trailer (direction present, count missing) is caught by
  `pq_getmsgint64`'s own insufficient-data check / `pq_getmsgend`, surfacing as
  a protocol violation — verify this yields a clean message; if not, add an
  explicit length check.
- The same length-detection idiom (`MyProcPort->protocol_cursor_enabled &&
  input_message->cursor < input_message->len`) is what `exec_bind_message()`
  uses for the Bind trailer; keep them consistent.

## B2. `exec_execute_message()` signature (~line 2108)

```c
static void
exec_execute_message(const char *portal_name, long max_rows,
                     bool is_scroll_execute, int fetch_direction,
                     int64 fetch_count)
```

(Keep `max_rows` as `long` — unchanged from today.)

## B3. `exec_execute_message()` body — routing (~line 2270)

Replace the single `PortalRun()` call site:

```c
if (max_rows <= 0)
    max_rows = FETCH_ALL;

if (is_scroll_execute)
{
    long    count;

    /* Validate direction. */
    if (fetch_direction != FETCH_FORWARD &&
        fetch_direction != FETCH_BACKWARD &&
        fetch_direction != FETCH_ABSOLUTE &&
        fetch_direction != FETCH_RELATIVE)
        ereport(ERROR,
                (errcode(ERRCODE_PROTOCOL_VIOLATION),
                 errmsg("invalid fetch direction in Execute message: %d",
                        fetch_direction)));

    /* Guard: non-forward fetch requires a fetchable strategy (B7). */
    if (fetch_direction != FETCH_FORWARD &&
        portal->strategy != PORTAL_ONE_SELECT &&
        portal->strategy != PORTAL_ONE_RETURNING &&
        portal->strategy != PORTAL_ONE_MOD_WITH &&
        portal->strategy != PORTAL_UTIL_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("cannot scroll a portal of this type")));

    /* Map Int64 wire count onto platform long (B4). */
    count = fetch_count_wire_to_long(fetch_count);

    nprocessed = PortalRunFetch(portal,
                                (FetchDirection) fetch_direction,
                                count,
                                receiver);

    /* Build tag preserving the query's verb (B5). */
    InitializeQueryCompletion(&qc);
    if (portal->qc.commandTag != CMDTAG_UNKNOWN)
    {
        CopyQueryCompletion(&qc, &portal->qc);
        qc.nprocessed = nprocessed;
    }
    receiver->rDestroy(receiver);
    error_context_stack = error_context_stack->previous;
    /* fetch always completes — never PortalSuspended (B5) */
    EndCommand(&qc, dest, false);
    /* CCI / xact handling as in the completed branch below */
}
else
{
    completed = PortalRun(portal, max_rows, true,
                          receiver, receiver, &qc);
    /* existing completed / PortalSuspended handling unchanged */
}
```

> Implementation note: the scroll branch shares the post-run xact bookkeeping
> (CommandCounterIncrement, XACT_FLAGS_PIPELINING, disable_statement_timeout)
> with the existing `completed` branch. Factor that tail so it runs for both
> paths rather than duplicating it; the only difference is scroll never emits
> PortalSuspended.

## B4. Count boundary helper

`PortalRunFetch(Portal, FetchDirection, long count, DestReceiver *)` takes
`count` as a C `long` — 64-bit on LP64, 32-bit on Windows LLP64. The wire field
is `Int64`. `FETCH_ALL == LONG_MAX` (`parsenodes.h:3553`), so we must never
synthesize that sentinel by truncation. Add a static helper in postgres.c:

```c
static long
fetch_count_wire_to_long(int64 count)
{
    /* Reserved token: portable "fetch all". */
    if (count == PG_INT64_MAX)          /* src/include/c.h:676 */
        return FETCH_ALL;               /* == LONG_MAX */

#if SIZEOF_LONG < 8
    if (count > LONG_MAX || count < LONG_MIN)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("fetch count out of range for this platform")));
#endif
    return (long) count;
}
```

Semantics (thin pass-through to `FetchStmt.howMany`):
- `PG_INT64_MAX` ⇒ `FETCH_ALL` — the only portable way to express "all".
- `0` ⇒ `howMany 0` = zero-row no-op, matching SQL `FETCH FORWARD 0`.
- other N ⇒ literal pass-through after range check.

## B5. Completion tag for the fetch path

Today's forward Execute tag is built in `PortalRun` (`pquery.c` ~line 763): for
the SELECT family it does `CopyQueryCompletion(qc, &portal->qc)` then
`qc->nprocessed = nprocessed`. The verb is the **original query's**
`portal->qc.commandTag` (e.g. `SELECT`), count overwritten. `PortalRunFetch`
returns a bare `uint64` (no QueryCompletion), so the fetch path builds the tag
itself, mirroring `PortalRun` — and **not** `PerformPortalFetch`, which
deliberately uses `CMDTAG_FETCH`/`CMDTAG_MOVE` (`portalcmds.c:216`) because it's
a SQL-level FETCH statement. Protocol-level scrollable Execute must preserve the
query's verb for wire compatibility (clients parse it).

`CopyQueryCompletion` is `src/include/tcop/cmdtag.h:45` (inline); `portal->qc`
is `src/include/utils/portal.h:138`.

**Suspend vs complete:** SQL FETCH always completes (count-bounded), so the
scroll path always sends CommandComplete, never PortalSuspended. The
`if (completed) … else PortalSuspended` logic stays exclusive to the non-scroll
`PortalRun` path.

## B6. No change to `execute_is_fetch` (~line 2225)

`execute_is_fetch = !portal->atStart` stays. `PortalRunFetch` maintains
`atStart`/`atEnd`/`portalPos`, so a later plain Execute behaves correctly.

## B7. Error model (guards)

`PortalRunFetch`'s `default:` arm is an internal `elog(ERROR, "unsupported
portal strategy")` (`pquery.c:1434`) — not client-facing. The B3 pre-check
raises `ERRCODE_FEATURE_NOT_SUPPORTED` first for non-forward fetch on a
non-fetchable strategy. Backward on a NO SCROLL portal already raises "cursor
can only scan forward" (`ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE`) from the
existing `PortalRunSelect()` path; surfaces unchanged, not duplicated.

---

## D (this commit). Documentation — `doc/src/sgml/protocol.sgml`

- Extend the Execute message description with the optional direction/count
  trailer.
- Extend the `_pq_.protocol_cursor` extension table entry to mention scrollable
  Execute.
- State the max_rows-ignored rule and the count range / `PG_INT64_MAX`-is-ALL
  sentinel rule.

---

## Build / verify (this commit)

- `meson setup build && meson compile -C build` (or `./configure && make`).
- No automated test in this commit (tests land in commit 3). Sanity: a normal
  forward Execute with no trailer must behave exactly as before (regression
  suite `meson test -C build` should be green).
- The trailer path is exercised end-to-end only once commits 2 (libpq) and 3
  (tests) land.

## Definition of done

- Builds clean.
- Existing regression tests pass unchanged (no-trailer path untouched).
- New code paths reachable only with `_pq_.protocol_cursor` negotiated +
  trailer present.
