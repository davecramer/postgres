# Design: Scrollable Execute for the extended-query protocol

Status: proposal / RFC for pgsql-hackers
Author: Dave Cramer
Targets: master (PostgreSQL 19+)
Companion: extends the `_pq_.protocol_cursor` extension introduced by the v4
Bind patch (`v4-0001-Add-_pq_.cursor-protocol-extension-for-cursor-opt.patch`).

## 1. Problem

The v4 Bind patch lets a client set `CURSOR_OPT_SCROLL` / `NO_SCROLL` / `HOLD`
on the portal a Bind creates, by appending an Int32 flag bitmap to the Bind
message when `_pq_.protocol_cursor` has been negotiated. That makes the portal
*scrollable*. But the Execute message can still only drive a portal **forward**:
`exec_execute_message()` calls `PortalRun()`, which has no notion of direction.

So today a client can build a scrollable portal it cannot scroll. To actually
fetch backward or jump to an absolute position, the client must abandon the
extended-query protocol and issue textual `DECLARE ... SCROLL CURSOR` +
`FETCH BACKWARD` / `FETCH ABSOLUTE` statements. That path:

- requires a textual SQL round-trip per fetch,
- does not compose with Bind parameters,
- does not share plan caching with extended-protocol queries, and
- forces drivers to maintain two parallel result-set strategies (forward-only
  vs. scrollable). pgjdbc's `TYPE_SCROLL_INSENSITIVE` buffers the entire result
  on the client today precisely because the server-side DECLARE/FETCH path is
  too awkward to reach from the driver API.

The backend already does all the work. `PortalRunFetch()`
(`src/backend/tcop/pquery.c`) implements every direction the SQL `FETCH`
grammar supports, for all four portal strategies that can produce rows. No
protocol message ever reaches it. This proposal adds that message path.

## 2. Proposal in one sentence

When `_pq_.protocol_cursor` is negotiated, an Execute message may carry two
trailing fields — an `Int32` direction and an `Int64` count — and the backend
routes such an Execute through `PortalRunFetch()` instead of `PortalRun()`.

## 3. Wire format

Existing Execute message body:

```
String  portal name
Int32   maximum number of rows (max_rows); 0 = no limit
```

Extended Execute message body (only when `_pq_.protocol_cursor` negotiated):

```
String  portal name
Int32   max_rows               (legacy field; see §5)
Int32   direction              (FetchDirection: 0 fwd, 1 back, 2 abs, 3 rel)
Int64   count                  (signed; SQL FETCH count semantics)
```

Detection is by message length, exactly as the Bind extension does it: after
reading portal name and max_rows, if `input_message.cursor < input_message.len`
there are trailing fields; read them. This is the same idiom
`exec_bind_message()` uses (`MyProcPort->protocol_cursor_enabled &&
input_message->cursor < input_message->len`).

Rules:

- **Extension not negotiated + trailing bytes present** → protocol violation
  (`ERRCODE_PROTOCOL_VIOLATION`). This is unchanged behavior: trailing bytes on
  Execute are illegal today.
- **Extension negotiated + no trailing bytes** → message keeps its exact
  current meaning: forward fetch of up to max_rows rows via `PortalRun()`.
- **Extension negotiated + both trailing fields present** → scrollable execute
  via `PortalRunFetch()`.

The two trailing fields are all-or-nothing: a partial trailer (direction but no
count) is a protocol violation.

### Direction encoding

The wire direction values are defined to equal the backend `FetchDirection`
enum (`src/include/nodes/parsenodes.h`):

| Wire | FetchDirection  | Meaning                          |
|------|-----------------|----------------------------------|
| 0    | FETCH_FORWARD   | next `count` rows                |
| 1    | FETCH_BACKWARD  | previous `count` rows            |
| 2    | FETCH_ABSOLUTE  | row at absolute position `count` |
| 3    | FETCH_RELATIVE  | row `count` relative to current  |

Any other value is `ERRCODE_PROTOCOL_VIOLATION`. The 1:1 mapping is deliberate:
the protocol is a thin pass-through to the same machinery SQL `FETCH` uses, not
a parallel dialect with its own quirks.

### Count encoding and the `long` boundary

`PortalRunFetch(Portal, FetchDirection, long count, DestReceiver *)` takes
`count` as a C `long`. On LP64 platforms `long` is 64-bit; on Windows LLP64
`long` is 32-bit. The wire field is `Int64` because SQL `FETCH` count is
logically 64-bit signed and we do not want the protocol to impose a narrower
range than `FETCH` itself.

To keep observable behavior platform-independent, the backend **range-checks**
the Int64 count on ingest: if it does not fit in `long` on the current
platform, raise `ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE`. We also map the
sentinels explicitly rather than letting them fall through arithmetic:

- `FETCH_ALL` is `LONG_MAX`. A wire count must not be silently reinterpreted as
  "all rows" by truncation. We document that wire count `0` with
  `FETCH_FORWARD`/`FETCH_BACKWARD` means "fetch all in that direction" (matching
  `FETCH ALL` / the `max_rows==0` convention), and we never derive `LONG_MAX`
  from a truncated 64-bit value.

(The cleaner long-term fix — widening `PortalRunFetch`/`DoPortalRunFetch` to
`int64` — is noted as future work in §8; it touches the SQL FETCH path too and
is out of scope for this patch.)

## 4. Backend routing

In `exec_execute_message()`, after the existing `max_rows <= 0` normalization:

- If a direction/count trailer was parsed, call `PortalRunFetch()` with the
  decoded direction and count, having first run the §6 guards.
- Otherwise, unchanged: call `PortalRun()`.

`execute_is_fetch = !portal->atStart` (the existing logic) stays correct:
`PortalRunFetch()` maintains `atStart`/`atEnd`/`portalPos` across reverse
fetches, so a subsequent plain Execute still detects "fetch from an
already-started portal" the same way.

## 5. Legacy max_rows interaction

When the trailing direction/count fields are present, **max_rows is ignored**
and this is documented. Rationale: the count field exists specifically to
express richer semantics than max_rows can, and the
extension-on + trailing-bytes-present signal is already unambiguous. We do *not*
error when both max_rows and count are non-zero — that would force clients to
track which field they last touched for no protocol benefit.

## 6. Errors and guards

`PortalRunFetch()` handles exactly four portal strategies: `PORTAL_ONE_SELECT`,
`PORTAL_ONE_RETURNING`, `PORTAL_ONE_MOD_WITH`, `PORTAL_UTIL_SELECT`. Its
`default:` arm is an internal `elog(ERROR, "unsupported portal strategy")` —
not a clean client-facing error. We therefore add **explicit pre-checks** in
the execute path so the client always gets a proper SQLSTATE:

1. **Wrong strategy**: if direction != FORWARD and the portal strategy is not
   one of the four, raise `ERRCODE_FEATURE_NOT_SUPPORTED`
   ("cannot scroll a portal of this type") before calling `PortalRunFetch()`.
   (Forward fetch on any strategy keeps going through `PortalRun()` as today.)
2. **Backward on NO SCROLL**: backward/relative-backward/absolute-backward on a
   non-scrollable portal already raises "cursor can only scan forward"
   (`ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE`) from inside the existing
   `PortalRunSelect()` path. That error surfaces unchanged. We do not duplicate
   it; we rely on the existing check, which already interprets NO SCROLL
   loosely (any direction is fine as long as net motion is forward).

## 7. Mid-fetch result and command tag

If the client asks for N rows and only M < N remain in the requested direction,
the backend sends M DataRows and a CommandComplete reporting the actual count,
exactly as SQL `FETCH` does. `PortalRunFetch()` returns the processed-row count,
which feeds the tag.

**Command-tag compatibility note (to verify on a real run):** extended-protocol
Execute today completes with the *original query's* command tag (e.g.
`SELECT 3`), not a `FETCH`-verb tag. The scrollable path must preserve that —
clients parse the verb. The row count in the tag should reflect rows actually
returned by this Execute, not the cursor's total.

## 8. Holdable portals (priority case)

`CURSOR_OPT_HOLD` materializes the result into the portal's tuplestore at
commit. After commit the portal's fetch behavior is the `PORTAL_UTIL_SELECT` /
`holdStore` path, which `PortalRunFetch()` already handles (it calls
`FillPortalStore()` if the store isn't built yet, then `DoPortalRunFetch()`).
This is the scenario where protocol-level scrollable execute most clearly beats
DECLARE/FETCH, and where today's Execute-only model is most obviously
insufficient. It gets dedicated test coverage (forward + backward fetch across a
commit boundary).

## 9. Negotiation: one extension, both halves

Bind cursor-options and scrollable Execute share the single
`_pq_.protocol_cursor` flag (negotiated via the `_pq_.`-prefixed startup-packet
option and `NegotiateProtocolVersion`, as the v4 patch already implements;
backend side sets `port->protocol_cursor_enabled`, client side
`conn->protocol_cursor_enabled`). They are not split into two negotiated
extensions.

Rationale: making a portal scrollable at Bind is only useful if you can drive it
scrollably at Execute. Splitting the negotiation creates a useless state where
one half is enabled and the other isn't. Two features under one flag is mildly
unusual but the coupling is real, not cosmetic.

## 10. libpq surface

- `int PQsendExecutePortal(PGconn *conn, const char *portalName, int direction,
  int64_t count)` — send an extended Execute (direction + count) for an
  existing portal. Requires the extension; returns 0 otherwise.
- `int PQsendQueryScrollable(...)` — convenience that does Bind (with cursor
  options) + Execute + Sync in one call.
- Direction constants in `libpq-fe.h`: `PQ_FETCH_FORWARD` (0),
  `PQ_FETCH_BACKWARD` (1), `PQ_FETCH_ABSOLUTE` (2), `PQ_FETCH_RELATIVE` (3).

This mirrors the v4 Bind-side surface (`PQsendBindWithCursorOptions`,
`PQPortalCursorEnabled`, `PQ_BIND_CURSOR_*`).

## 11. PgBouncer / pooling

For non-HOLD portals there is no concern: a portal lives within a transaction,
and a transaction is served by one backend, so all Execute messages for that
portal hit the same backend. The only interesting case is `WITH HOLD` across a
commit under transaction pooling — and that is already the client's
responsibility today for DECLARE ... WITH HOLD; this proposal does not change
it. Documented as such.

## 12. Alternatives considered

- **Int32 count** (match max_rows width): rejected — caps absolute/relative
  positions and fetch sizes at ~2.1B, reintroducing the narrowness we are trying
  to remove.
- **Widen PortalRunFetch to int64 now**: deferred — correct end state, but a
  signature change to core functions shared with the SQL FETCH path; larger
  review surface than this protocol feature needs.
- **Separate extension flag for Execute**: rejected, see §9.
- **Error when max_rows and count both set**: rejected, see §5.
- **A brand-new message type instead of extending Execute**: rejected — reuses
  none of the existing Execute plumbing (statement timeout, logging,
  `execute_is_fetch`) and fragments the protocol for no gain.

## 13. Open items — RESOLVED from source (see SPEC §H)

1. **CommandComplete tag** (§7): the fetch path preserves the *query's* verb
   (`SELECT <n>`), not `FETCH`, by copying `portal->qc` and overwriting
   nprocessed — exactly what `PortalRun` does today. SQL-level FETCH's
   `CMDTAG_FETCH` is deliberately NOT used. Scrollable Execute always completes
   (never PortalSuspended).
2. **Count semantics**: thin pass-through to `FetchStmt.howMany`. Wire `0` =
   zero-row no-op (== SQL `FETCH FORWARD 0`); positive/negative N passed through
   after a platform-`long` range check; `PG_INT64_MAX` is the reserved,
   portable "ALL" token mapping to `FETCH_ALL`. Exposed to clients as
   `PQ_FETCH_ALL`.
3. **64-bit wire helpers**: backend `pq_getmsgint64` exists; frontend gets a new
   `pqPutInt64` (pg_hton64 is available frontend-side).
