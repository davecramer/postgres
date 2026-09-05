# Spec — Commit 1/3: Backend (validate and plan Bind cursor options)

Fixes F1, F2, F3, F4 and F7 from `CURSOR_OPTIONS_FIX_SPEC.md`, plus the
Execute-trailer wire decoupling and message-style cleanups. Read the index's
normative rule set first — this file implements it.

**Scope of this commit:** `src/backend/utils/cache/plancache.c`,
`src/include/utils/plancache.h`, `src/backend/tcop/postgres.c`. No wire-format
change. No libpq change. No SGML (that is commit 2). No new tests (commit 3).

Line numbers are anchors against the current working tree, not literals.

---

## A. Behavior change summary (normative)

| Bind flags | Query | Before | After |
|------------|-------|--------|-------|
| `HOLD` | `INSERT ... RETURNING`, `INSERT`, `SHOW` | `SIGSEGV` at COMMIT | `ERROR` at Bind, `0A000` |
| `SCROLL` | `SELECT id, count(*) ... GROUP BY id` | wrong rows on backward fetch | correct rows (Material added by planner) |
| `SCROLL` | `SELECT 1` | zero rows on backward fetch | correct rows |
| `SCROLL` | `INSERT ...` | silently ignored | `ERROR` at Bind, `0A000` |
| `SCROLL` | `SELECT ... FOR UPDATE` | accepted | `ERROR` at Bind, `0A000` |
| `HOLD` alone | any `SELECT` | backward fetch silently permitted, wrong rows | backward fetch rejected (`NO_SCROLL` default retained) |
| `SCROLL｜NO_SCROLL` | any | accepted, every backward fetch errors | `ERROR` at Bind, `34000`-class |
| `0` | any | portal defaults retained | unchanged |

Nothing changes for a connection that has not negotiated
`_pq_.protocol_cursor`, and nothing changes for a Bind that sends flags `0`.

---

## B1. `plancache`: build a plan with caller-supplied cursor options

`src/include/utils/plancache.h`, at the existing `GetCachedPlan` declaration
(`plancache.h:240`):

```c
extern CachedPlan *GetCachedPlan(CachedPlanSource *plansource,
								 ParamListInfo boundParams,
								 ResourceOwner owner,
								 QueryEnvironment *queryEnv);
extern CachedPlan *GetCachedPlanExtraOptions(CachedPlanSource *plansource,
											 ParamListInfo boundParams,
											 ResourceOwner owner,
											 QueryEnvironment *queryEnv,
											 int extra_cursor_options);
```

`src/backend/utils/cache/plancache.c`, static prototype at `plancache.c:101`
and definition at `:1045` — `BuildCachedPlan` gains the parameter and applies it
to the planner call at `:1101`:

```c
static CachedPlan *BuildCachedPlan(CachedPlanSource *plansource, List *qlist,
								   ParamListInfo boundParams,
								   QueryEnvironment *queryEnv,
								   int extra_cursor_options);
```

```c
	/*
	 * Generate the plan.  extra_cursor_options is supplied by the caller for
	 * plans that must satisfy a per-execution requirement not recorded in the
	 * CachedPlanSource, currently only CURSOR_OPT_SCROLL; such plans are never
	 * saved as the generic plan (see GetCachedPlanExtraOptions).
	 */
	plist = pg_plan_queries(qlist, plansource->query_string,
							plansource->cursor_options | extra_cursor_options,
							boundParams);
```

Rename the existing `GetCachedPlan` body to `GetCachedPlanExtraOptions`, add the
parameter, and leave `GetCachedPlan` as a thin wrapper:

```c
CachedPlan *
GetCachedPlan(CachedPlanSource *plansource, ParamListInfo boundParams,
			  ResourceOwner owner, QueryEnvironment *queryEnv)
{
	return GetCachedPlanExtraOptions(plansource, boundParams, owner, queryEnv,
									 0);
}
```

Inside `GetCachedPlanExtraOptions`, two changes. The plan-choice decision at
`plancache.c:1324`:

```c
	/*
	 * Decide whether to use a custom plan.  A plan built with extra cursor
	 * options is not interchangeable with the plans this CachedPlanSource
	 * caches, so it must always be a custom plan.
	 */
	if (extra_cursor_options != 0)
		customplan = true;
	else
		customplan = choose_custom_plan(plansource, boundParams);
```

and the custom-plan branch at `:1381`:

```c
	if (customplan)
	{
		/* Build a custom plan */
		plan = BuildCachedPlan(plansource, qlist, boundParams, queryEnv,
							   extra_cursor_options);

		/*
		 * Don't fold a plan built with extra cursor options into the cost
		 * averages that drive choose_custom_plan(): it may carry a Material
		 * node that an ordinary plan for this statement would not have, and it
		 * was not chosen on cost grounds in the first place.
		 */
		if (extra_cursor_options == 0)
		{
			plansource->total_custom_cost += cached_plan_cost(plan, true);
			plansource->num_custom_plans++;
		}
	}
```

Update the two internal `BuildCachedPlan` call sites (`:1338` generic, `:1383`
custom) for the new signature; the generic one passes `0`.

> `plansource->gplan` is untouched on the custom path, so the extra-options plan
> is never handed to a later Bind of the same prepared statement. The plan is
> refcounted and reparented exactly like any other custom plan, so no lifetime
> handling changes.

---

## B2. `postgres.c`: decode the Bind flags without mutating the portal

Replace the block at `postgres.c:2047-2085`. The flags are decoded into a local;
nothing is applied to the portal until the plan exists (§B4).

```c
	/*
	 * Get bind cursor-option flags if present (_pq_.protocol_cursor enabled).
	 *
	 * The wire-level flag values (PQ_BIND_CURSOR_*) are defined independently
	 * of the server-internal CURSOR_OPT_* constants in parsenodes.h, so we
	 * must map between the two representations here.  The flags are not
	 * applied to the portal yet: SCROLL is a planner input and both SCROLL and
	 * HOLD depend on the portal strategy, neither of which is known until the
	 * plan has been obtained.  See apply_bind_cursor_options().
	 */
	if (MyProcPort != NULL && MyProcPort->protocol_cursor_enabled &&
		input_message->cursor < input_message->len)
	{
		int			bind_ext_flags;

		bind_ext_flags = pq_getmsgint(input_message, 4);

		/* Reject any bits we don't recognize */
		if (bind_ext_flags & ~PQ_CURSOR_FLAG_ALL)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unrecognized cursor option flags in Bind message: 0x%x",
							bind_ext_flags & ~PQ_CURSOR_FLAG_ALL)));

		if (bind_ext_flags & PQ_CURSOR_FLAG_SCROLL)
			bind_cursor_options |= CURSOR_OPT_SCROLL;
		if (bind_ext_flags & PQ_CURSOR_FLAG_NO_SCROLL)
			bind_cursor_options |= CURSOR_OPT_NO_SCROLL;
		if (bind_ext_flags & PQ_CURSOR_FLAG_HOLD)
			bind_cursor_options |= CURSOR_OPT_HOLD;

		/* Mirrors transformDeclareCursorStmt(); see analyze.c */
		if ((bind_cursor_options & CURSOR_OPT_SCROLL) &&
			(bind_cursor_options & CURSOR_OPT_NO_SCROLL))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
			/* translator: %s is a SQL keyword */
					 errmsg("cannot specify both %s and %s",
							"SCROLL", "NO SCROLL")));
	}
	pq_getmsgend(input_message);
```

New local in `exec_bind_message`, beside the other declarations:

```c
	int			bind_cursor_options = 0;
	int			plan_cursor_options = 0;
```

Server-side names for the wire values, so the literals `0x0001`/`0x0007` and the
raw direction numbers stop appearing in comments. Add to
`src/include/libpq/pqcomm.h` next to the other protocol constants:

```c
/*
 * Wire values for the _pq_.protocol_cursor extension: the cursor-option bitmap
 * carried by the optional Int32 trailer of the Bind message, and the fetch
 * direction carried by the optional trailer of the Execute message.
 *
 * These are protocol constants.  They intentionally do not track the
 * server-internal CURSOR_OPT_* constants (parsenodes.h) or the FetchDirection
 * enum, so those can be reordered or extended freely.  The client-side names in
 * libpq-fe.h (PQ_BIND_CURSOR_*, PQ_FETCH_*) must carry the same values.
 */
#define PQ_CURSOR_FLAG_SCROLL		0x0001
#define PQ_CURSOR_FLAG_NO_SCROLL	0x0002
#define PQ_CURSOR_FLAG_HOLD			0x0004
#define PQ_CURSOR_FLAG_ALL			0x0007

#define PQ_CURSOR_FETCH_FORWARD		0
#define PQ_CURSOR_FETCH_BACKWARD	1
#define PQ_CURSOR_FETCH_ABSOLUTE	2
#define PQ_CURSOR_FETCH_RELATIVE	3
```

> Distinct names, not the public `PQ_BIND_CURSOR_*` spellings: `libpq-int.h`
> includes both `libpq-fe.h` and `libpq/pqcomm.h`, so reusing the public names
> here would be a macro redefinition in every libpq translation unit.

> `errcode(ERRCODE_INVALID_CURSOR_DEFINITION)` and the exact `errmsg` text are
> copied from `analyze.c:3369-3375` so the existing translation is reused. F4 is
> fixed here; the `ereport` is inside the flags block, which is the only place
> the combination can arise.

Also fix F7's second site while in the file: `postgres.c:5147` (§B5).

---

## B3. `postgres.c`: plan a scrollable portal with `CURSOR_OPT_SCROLL`

At `postgres.c:2088-2093`, replace the `GetCachedPlan` call:

```c
	/*
	 * If the client asked for a scrollable portal, the plan must be built with
	 * CURSOR_OPT_SCROLL so that standard_planner() adds a Material node when
	 * the top plan cannot be scanned backwards; PortalStart() will pass
	 * EXEC_FLAG_BACKWARD to the executor for such a portal.  This is the same
	 * contract PerformCursorOpen() gets by passing cstmt->options to
	 * pg_plan_query() (portalcmds.c:102).
	 *
	 * Only SELECTs are worth planning this way: no other command tag can
	 * produce a PORTAL_ONE_SELECT portal, and apply_bind_cursor_options()
	 * rejects everything else below.
	 */
	if ((bind_cursor_options & CURSOR_OPT_SCROLL) &&
		psrc->commandTag == CMDTAG_SELECT)
		plan_cursor_options = CURSOR_OPT_SCROLL;

	/*
	 * Obtain a plan from the CachedPlanSource.  Any cruft from (re)planning
	 * will be generated in MessageContext.  The plan refcount will be
	 * assigned to the Portal, so it will be released at portal destruction.
	 */
	cplan = GetCachedPlanExtraOptions(psrc, params, NULL, NULL,
									  plan_cursor_options);
```

> Consequence to state in the commit message: a Bind that requests `SCROLL`
> always replans, because the resulting plan cannot be cached. That is the same
> cost `DECLARE ... SCROLL CURSOR` pays, and it only applies to Binds that ask
> for scroll.

---

## B4. `postgres.c`: apply and validate the options against the plan

New static function, placed just above `exec_bind_message`:

```c
/*
 * apply_bind_cursor_options
 *
 * Apply the cursor options requested by a Bind message (already mapped to
 * CURSOR_OPT_* values) to a portal whose query has been defined but not yet
 * started.  Rejects combinations the portal machinery cannot honor, mirroring
 * the checks transformDeclareCursorStmt() and PerformCursorOpen() apply to
 * DECLARE CURSOR.
 *
 * Must be called after PortalDefineQuery() -- the plan is needed -- and before
 * PortalStart(), which consumes portal->cursorOptions to decide whether to
 * start the executor with EXEC_FLAG_BACKWARD.
 */
static void
apply_bind_cursor_options(Portal portal, int cursor_options)
{
	/* No flags requested: keep the defaults CreatePortal() installed. */
	if (cursor_options == 0)
		return;

	if (cursor_options & (CURSOR_OPT_SCROLL | CURSOR_OPT_HOLD))
	{
		PlannedStmt *pstmt;

		/*
		 * Both options require a live QueryDesc: PortalStart() only builds one
		 * for PORTAL_ONE_SELECT (pquery.c:492), and PersistHoldablePortal()
		 * dereferences it unconditionally (portalcmds.c:340).  This is also the
		 * only strategy DECLARE CURSOR can produce (portalcmds.c:159).
		 */
		if (ChoosePortalStrategy(portal->stmts) != PORTAL_ONE_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot use cursor options with this query"),
					 errdetail("Scrollable and holdable portals are supported only for a single SELECT statement.")));

		/* PORTAL_ONE_SELECT implies a single non-utility PlannedStmt. */
		pstmt = linitial_node(PlannedStmt, portal->stmts);

		if (pstmt->rowMarks != NIL)
		{
			if (cursor_options & CURSOR_OPT_SCROLL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot create a scrollable portal for a query with a row locking clause"),
						 errdetail("Scrollable cursors must be READ ONLY.")));
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create a holdable portal for a query with a row locking clause"),
					 errdetail("Holdable cursors must be READ ONLY.")));
		}

		/*
		 * Can't happen: exec_bind_message() plans a scrollable portal with
		 * CURSOR_OPT_SCROLL, so the planner has already materialized the top
		 * plan at need.  Check anyway rather than let the executor be started
		 * with EXEC_FLAG_BACKWARD over a plan that cannot honor it.
		 */
		if ((cursor_options & CURSOR_OPT_SCROLL) &&
			!ExecSupportsBackwardScan(pstmt->planTree))
			elog(ERROR, "portal plan does not support backward scan");
	}

	/*
	 * CreatePortal() defaults to CURSOR_OPT_NO_SCROLL (portalmem.c:217).  An
	 * explicit SCROLL request replaces that default; anything else is merely
	 * added, so that (for example) requesting HOLD alone does not quietly make
	 * a portal backward-fetchable.
	 */
	if (cursor_options & CURSOR_OPT_SCROLL)
		portal->cursorOptions &= ~CURSOR_OPT_NO_SCROLL;

	portal->cursorOptions |= cursor_options;
}
```

Call it in `exec_bind_message`, after the `PortalDefineQuery` block and the
`planId` loop (`postgres.c:2118`) and before `PortalStart` (`:2127`):

```c
	/*
	 * Apply any cursor options the Bind message requested.  This has to happen
	 * after the portal's query is defined (the plan and strategy are needed)
	 * and before PortalStart(), which reads portal->cursorOptions.
	 */
	apply_bind_cursor_options(portal, bind_cursor_options);

	/* Done with the snapshot used for parameter I/O and parsing/planning */
	if (snapshot_set)
		PopActiveSnapshot();
```

`postgres.c` needs `#include "executor/executor.h"` for
`ExecSupportsBackwardScan` (`executor.h:110`) and `#include "tcop/pquery.h"`
for `ChoosePortalStrategy` (`pquery.h:26`); check the existing include list
before adding, `pquery.h` is likely already there.

> Placement is deliberate on both sides. It is after `PortalDefineQuery`, so it
> is outside the "DO NOT put any code that could possibly throw an error"
> window at `postgres.c:2098` — by then the plan's refcount belongs to the
> portal and ordinary error cleanup releases it. It is before `PortalStart`, so
> no executor has been started when the error is thrown.
>
> Why not defer the strategy test to after `PortalStart`: `ChoosePortalStrategy`
> is a pure function of `portal->stmts` (`pquery.c:206`) and `PortalStart` calls
> it itself at `:464`, so calling it early costs nothing and lets us error out
> before the executor is started. Erroring after `PortalStart` would leave a
> started-but-doomed executor to be torn down by transaction abort.
>
> Note the resulting error ordering: an unrecognized flag or the
> `SCROLL｜NO_SCROLL` combination is reported before the query is planned, while
> a strategy or row-mark violation is reported after. Both are Bind-time errors
> from the client's point of view.

---

## B5. `postgres.c`: Execute trailer (uncommitted work)

Three changes at the `PqMsg_Execute` case, `postgres.c:5123-5160`.

1. F7: guard `MyProcPort`.
2. Message style: primary message states the problem, detail carries the
   requirement; extension name double-quoted.
3. Decode the direction from wire values instead of casting onto
   `FetchDirection`, matching what the Bind bitmap already does.

```c
					/*
					 * Optional scrollable-execute trailer, present only when
					 * the _pq_.protocol_cursor extension has been negotiated.
					 * Any trailing bytes without the extension are a protocol
					 * violation.
					 */
					if (input_message.cursor < input_message.len)
					{
						if (MyProcPort == NULL ||
							!MyProcPort->protocol_cursor_enabled)
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid Execute message"),
									 errdetail("Trailing data in an Execute message requires the \"_pq_.protocol_cursor\" protocol extension.")));

						wire_direction = pq_getmsgint(&input_message, 4);
						fetch_count = pq_getmsgint64(&input_message);
						is_scroll_execute = true;
					}
					pq_getmsgend(&input_message);
```

The direction wire constants come from `pqcomm.h` (§B2); add an explicit mapping
helper next to `fetch_count_wire_to_long` (`postgres.c:2184`):

```c
/*
 * fetch_direction_wire_to_enum
 *
 * Map the Int32 fetch direction from the scrollable-Execute wire trailer onto
 * the server's FetchDirection enum.  Keeping the two independent means the enum
 * can be reordered or extended without breaking the protocol.
 */
static FetchDirection
fetch_direction_wire_to_enum(int wire_direction)
{
	switch (wire_direction)
	{
		case PQ_CURSOR_FETCH_FORWARD:
			return FETCH_FORWARD;
		case PQ_CURSOR_FETCH_BACKWARD:
			return FETCH_BACKWARD;
		case PQ_CURSOR_FETCH_ABSOLUTE:
			return FETCH_ABSOLUTE;
		case PQ_CURSOR_FETCH_RELATIVE:
			return FETCH_RELATIVE;
	}

	ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("invalid fetch direction in Execute message: %d",
					wire_direction)));
	return FETCH_FORWARD;		/* keep compiler quiet */
}
```

`exec_execute_message` takes the wire value and converts, replacing the
open-coded validation at `postgres.c:2398-2406`:

```c
	if (is_scroll_execute)
	{
		FetchDirection direction;
		long		count;

		direction = fetch_direction_wire_to_enum(wire_direction);

		/* A scrollable Execute requires a fetchable portal. */
		if (portal->strategy != PORTAL_ONE_SELECT &&
			portal->strategy != PORTAL_ONE_RETURNING &&
			portal->strategy != PORTAL_ONE_MOD_WITH &&
			portal->strategy != PORTAL_UTIL_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot scroll a portal of this type")));

		/* Map Int64 wire count onto platform long. */
		count = fetch_count_wire_to_long(fetch_count);

		nprocessed = PortalRunFetch(portal, direction, count, receiver);
		...
```

Rename the parameter and locals accordingly: `int fetch_direction` becomes
`int wire_direction` in `exec_execute_message`'s signature and in the
`PqMsg_Execute` case (`:5127`), initialized to `PQ_CURSOR_FETCH_FORWARD`.

> The strategy list here is intentionally wider than §B4's: a scrollable
> Execute against a portal that was materialized by `FillPortalStore` is served
> from the hold tuplestore by `DoPortalRunFetch` and does not depend on
> executor backward-scan support. `PortalRunSelect`'s `NO_SCROLL` check
> (`pquery.c:933`) still applies, so a backward Execute on a portal that never
> requested `SCROLL` is rejected there, as it is for SQL `FETCH`.

Finally, drop the internal spec tags from the comments added by the uncommitted
work — `(B7)`, `(B4)`, `(B5)` at `postgres.c:2408`, `:2417`, `:2426` — they mean
nothing outside this repo's spec files.

---

## Build / verify (this commit)

```sh
ninja -C build18new
ninja -C build18new install
meson test -C build18new --suite setup --suite regress --suite isolation
meson test -C build18new libpq_protocol_cursor / 001_libpq_protocol_cursor
```

`libpq_protocol_cursor` case `dml_with_cursor_options` is *expected to fail*
after this commit: it asserts that `SCROLL` on an `INSERT` is silently ignored,
which rule 4 now rejects. Commit 3 updates it (§T0 in file 03). Do not "fix" it
by relaxing rule 4.

Manual reproducers (all against a **non-assert** build, since two of the three
crash shapes are Asserts elsewhere):

```sh
# F1 — each of these SIGSEGV'd the backend before this commit
for s in "INSERT INTO t VALUES (9) RETURNING id" "INSERT INTO t VALUES (9)" "SHOW work_mem"; do
    ./probe3 "$CONNINFO" "$s"     # Bind(HOLD) then COMMIT
done
# expect: ERROR 0A000, backend alive, no cluster recovery in the log

# F2 — differential against the SQL cursor
./probe2 "$CONNINFO"
# expect: Bind(SCROLL)+FETCH 2+FETCH BACKWARD 1 agrees with
#         DECLARE c SCROLL CURSOR for HashAgg, DISTINCT, "SELECT 1", UNION ALL

# F3 — HOLD alone must not enable backward fetch
# expect: FETCH BACKWARD 1 -> ERROR "cursor can only scan forward"

# F4
# expect: Bind(SCROLL|NO_SCROLL) -> ERROR "cannot specify both SCROLL and NO SCROLL"
```

Check `postmaster.log` for `terminated by signal 11` after the whole run — there
must be none.

---

## Definition of done

- [ ] `GetCachedPlanExtraOptions` added; `GetCachedPlan` is a wrapper; all
      `BuildCachedPlan` call sites updated; no behavior change for
      `extra_cursor_options == 0` (regress + isolation green).
- [ ] `MyProcPort` guarded at both `postgres.c:2054` and `:5147`.
- [ ] Bind flags decoded into a local and applied only via
      `apply_bind_cursor_options`; `portal->cursorOptions` is never assigned
      `0`.
- [ ] `SCROLL｜NO_SCROLL` rejected with the same message text as `analyze.c`.
- [ ] `SCROLL` or `HOLD` on a non-`PORTAL_ONE_SELECT` portal errors at Bind.
- [ ] `SCROLL` or `HOLD` on a query with row marks errors at Bind, reusing the
      existing errdetail strings.
- [ ] `SCROLL` Bind produces a plan that passes `ExecSupportsBackwardScan`, and
      that plan is not installed as `plansource->gplan` (verify: Bind the same
      prepared statement twice, once with `SCROLL` and once without, and confirm
      via `EXPLAIN`-equivalent inspection or a plan-cache mode test that the
      non-scroll Bind is not given a Material node).
- [ ] Execute-trailer direction decoded through `fetch_direction_wire_to_enum`;
      no cast from the wire value to `FetchDirection` remains.
- [ ] Execute trailing-data message follows message style
      (primary + errdetail, extension name double-quoted).
- [ ] `B4`/`B5`/`B7` tags removed from code comments.
- [ ] No new compiler warnings; `pgindent` clean on both touched files.
