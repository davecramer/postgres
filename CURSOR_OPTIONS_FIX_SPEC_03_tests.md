# Spec — Commit 3/3: Tests

Regression coverage for every finding in `CURSOR_OPTIONS_FIX_SPEC.md`. Depends
on commits 1 and 2.

**Scope of this commit:** `src/test/modules/libpq_protocol_cursor/libpq_protocol_cursor.c`,
`src/test/modules/libpq_protocol_cursor/t/001_libpq_protocol_cursor.pl`. No
backend, libpq or doc changes.

Line numbers are anchors against the current working tree, not literals.

---

## Why the existing suite missed all of this

The eight existing cases (`libpq_protocol_cursor.c:675-684`) pass against the
unfixed code. Three reasons, each of which the new cases are designed to remove:

1. **Every `SCROLL` case uses `ORDER BY`** (`:225`, and likewise in
   `holdable_scroll_cursor`). A Sort node supports backward scan, so the missing
   Material node (F2) is invisible. No case uses an aggregate, `DISTINCT`, or a
   bare `SELECT 1`.
2. **No case commits a transaction that holds a portal over a non-`SELECT`
   statement**, which is the only thing F1 needs — `test_dml_with_cursor_options`
   binds `INSERT` with `SCROLL` but never `HOLD`, and never commits.
3. **No case asserts a negative**: that a portal *without* `SCROLL` refuses to
   fetch backwards. `no_scroll_cursor` asserts it for an explicit `NO_SCROLL`,
   never for the default (F3).

Add, in `print_test_list` order, the cases below. Follow the file's existing
conventions: one `static void test_<name>(PGconn *conn)` per case, `pg_fatal`
on any unexpected status, pipeline mode plus `confirm_result_status` /
`consume_result_status` / `consume_null_result` for the Bind+Describe protocol
dance, and a `fprintf(stderr, "test_<name>... ")` / `"ok\n"` pair around the
body. Register each in `print_test_list` (`:675`) and in `main`'s dispatch chain
(`:722-739`, kept alphabetical).

---

## T0. Update `dml_with_cursor_options` (behavior change from commit 1)

`libpq_protocol_cursor.c:483-538` currently asserts that `SCROLL` on an `INSERT`
is "harmlessly ignored" and that Bind succeeds. Rule 4 makes that an error.
Rename to `dml_cursor_options_rejected` and invert the expectation:

```c
	/* SCROLL on a DML statement cannot be honored and must be rejected */
	if (PQsendBindWithCursorOptions(conn, "dmlstmt", 0, NULL, NULL, NULL, 0,
									"dmlportal", PQ_BIND_CURSOR_SCROLL) != 1)
		pg_fatal("PQsendBindWithCursorOptions failed: %s", PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	res = confirm_result_status(conn, PGRES_FATAL_ERROR);
	if (strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE), "0A000") != 0)
		pg_fatal("expected SQLSTATE 0A000, got %s",
				 PQresultErrorField(res, PG_DIAG_SQLSTATE));
	PQclear(res);
	consume_null_result(conn);
```

Keep the tail of the existing case that verifies the `INSERT` did not execute.
The rename means the `tests` list changes, which the TAP driver picks up
automatically (`001_libpq_protocol_cursor.pl:14-17`).

> The `SET lc_messages TO "C"` in `main` (`:719`) makes message-text assertions
> safe, but assert on `PG_DIAG_SQLSTATE` where a code exists and only on text
> where the code is not specific enough to distinguish the cases.

## T1. `hold_on_non_select_rejected` (F1)

The crash reproducer. For each statement below: `BEGIN`, prepare, Bind with
`PQ_BIND_CURSOR_HOLD`, expect `PGRES_FATAL_ERROR` with SQLSTATE `0A000` at Bind,
then `ROLLBACK` and assert `PQstatus(conn) == CONNECTION_OK`.

| Statement | Strategy before the fix | Symptom before the fix |
|-----------|------------------------|------------------------|
| `INSERT INTO t VALUES (9) RETURNING id` | `PORTAL_ONE_RETURNING` | `SIGSEGV` at COMMIT |
| `INSERT INTO t VALUES (9)` | `PORTAL_MULTI_QUERY` | `SIGSEGV` at COMMIT |
| `SHOW work_mem` | `PORTAL_UTIL_SELECT` | `SIGSEGV` at COMMIT |
| `WITH c AS (INSERT INTO t VALUES (9) RETURNING id) SELECT * FROM c` | `PORTAL_ONE_MOD_WITH` | untested territory |

Then, for the same four statements, repeat with a `COMMIT` instead of a
`ROLLBACK` after the failed Bind, and assert the connection survives the commit.
That is the exact sequence that killed the backend: the error at Bind now
prevents the portal from ever carrying `CURSOR_OPT_HOLD` into
`PreCommit_Portals` (`portalmem.c:724`).

## T2. `scroll_on_non_select_rejected` (F1/F2, `SCROLL` half)

Same four statements, `PQ_BIND_CURSOR_SCROLL` instead of `HOLD`, same
expectation. This is the case `dml_with_cursor_options` used to assert the
opposite of; T0 covers the plain `INSERT`, so T2 exists for the other three
strategies.

## T3. `scroll_backward_matches_sql_cursor` (F2 — the important one)

A differential test: the protocol path must return what the SQL path returns.
For each query, run both and compare row-for-row.

```c
static const char *const scroll_queries[] = {
	"SELECT id FROM t ORDER BY id",					/* Sort: was already OK */
	"SELECT id, count(*) FROM t GROUP BY id",		/* HashAgg: no backward scan */
	"SELECT DISTINCT id FROM t",					/* Unique/HashAgg */
	"SELECT 1",										/* Result */
	"SELECT id FROM t UNION ALL SELECT id FROM t",	/* Append */
	"SELECT id FROM t"								/* SeqScan: backward-safe */
};
```

For each: `DECLARE ref SCROLL CURSOR FOR <q>` plus Bind(`SCROLL`) on the same
`<q>` into portal `p`, then the same fetch script against both:

```
FETCH 2          -- move off atStart, otherwise backward is a documented no-op
FETCH BACKWARD 1
FETCH ABSOLUTE 1
FETCH RELATIVE 2
FETCH BACKWARD ALL
```

Compare tuple counts and every value. Before commit 1, the `GROUP BY` query
returns `1` where the reference cursor returns `3`, and `SELECT 1` returns zero
rows where the reference returns one; after it, all six must agree.

> The forward `FETCH 2` first is not incidental: a portal sitting at `atStart`
> treats a backward fetch as a no-op, which is what made the first version of
> this probe report a false pass.

## T4. `hold_only_keeps_no_scroll` (F3)

Bind with `PQ_BIND_CURSOR_HOLD` alone (no `SCROLL`) on a plain `SELECT id FROM
t`, `FETCH 2`, then `FETCH BACKWARD 1` and require an error whose message is
`cursor can only scan forward` (SQLSTATE `55000`), i.e. the same refusal a
`DECLARE CURSOR` without `SCROLL` gives (`pquery.c:933`). Before commit 1 the
backward fetch succeeded and returned a wrong row.

Then `COMMIT` and assert the portal is still usable afterwards with a forward
`FETCH` — the point of `HOLD` — and that the hold tuplestore was created without
random access (observable only as the continued refusal of `FETCH BACKWARD`
after commit; assert that too).

## T5. `scroll_and_no_scroll_rejected` (F4)

`PQsendBindWithCursorOptions(..., PQ_BIND_CURSOR_SCROLL | PQ_BIND_CURSOR_NO_SCROLL)`
must return `0` without a round trip, with `PQerrorMessage` mentioning both
constants (commit 2, §C2). Model the case on `test_invalid_flags_rejected`
(`:643`), which already asserts a client-side rejection.

> The corresponding *server-side* check (commit 1, §B2) is unreachable through
> libpq once §C2 lands, and the module has no raw-protocol facility. Cover it
> manually with the raw-socket script pattern used during the review
> (`Conn`/`msg`/`cstr` over `AF_UNIX`, simple-query `BEGIN`, then
> Parse/Bind(flags=0x3)/Sync) and record the expected result in the commit
> message: `ERROR 34000`-class, `cannot specify both SCROLL and NO SCROLL`. If a
> raw-protocol test helper lands in `src/test/modules/` later, promote this to an
> automated case.

## T6. `locking_clause_rejected` (rule 5)

`SELECT id FROM t FOR UPDATE` bound with `PQ_BIND_CURSOR_SCROLL`, then with
`PQ_BIND_CURSOR_HOLD`. Both must fail at Bind with `0A000` and the errdetail
strings the SQL path uses ("Scrollable cursors must be READ ONLY." /
"Holdable cursors must be READ ONLY."), matching what
`DECLARE SCROLL CURSOR ... FOR UPDATE` reports (`analyze.c:3403-3424`).
Assert on `PG_DIAG_MESSAGE_DETAIL` to distinguish the two.

## T7. `scroll_does_not_poison_plan_cache` (commit 1, §B1/§B3)

Prepare one statement whose plan differs under `SCROLL` — `SELECT id, count(*)
FROM t GROUP BY id` — then, on one connection:

1. Bind it without cursor options into `p1`; `FETCH 2`; `FETCH BACKWARD 1` must
   error (`cursor can only scan forward`).
2. Bind it with `SCROLL` into `p2`; the same fetch script must succeed and agree
   with the reference cursor.
3. Repeat steps 1 and 2 six more times, alternating, so the plan cache passes
   its five-custom-plan threshold (`plancache.c:1213`) and would switch to a
   generic plan if one had been cached.

Step 1 must keep failing and step 2 must keep succeeding throughout. If the
`SCROLL` plan were installed as `plansource->gplan`, step 1 would start
succeeding; if the non-scroll generic plan were reused for step 2, step 2 would
start returning the wrong rows.

## T8. `bind_param_limits` (F5)

`PQsendBindWithCursorOptions` with `nParams = -1` and with `nParams = 65536`
must both return `0` with the message `number of parameters must be between 0
and 65535`, and the connection must remain usable (`PQstatus` still
`CONNECTION_OK`, a following `PQexec("SELECT 1")` succeeds) — proving the
malformed Bind never reached the wire.

## T9. Scrollable-Execute cases (deferred)

The Execute-trailer changes from commit 1 §B5 (direction mapping, invalid
direction, partial trailer, one extra byte, count sentinel per direction,
trailing data without the extension) cannot be driven from this module until the
libpq API from `SCROLLABLE_EXECUTE_SPEC_02_libpq.md` exists. They belong to that
series' test file (`SCROLLABLE_EXECUTE_SPEC_03_tests.md`, the 11-case matrix).
Two additions to that matrix follow from this review and should be recorded
there now:

- `PG_INT64_MAX` with `ABSOLUTE` and with `RELATIVE` returns **zero** rows and
  leaves the portal at the end — not "all rows" (see file 02, §D4).
- A backward or absolute scrollable Execute against a portal bound *without*
  `SCROLL` must fail with `cursor can only scan forward`, not return rows.

---

## TAP driver changes

`t/001_libpq_protocol_cursor.pl`:

1. The existing loop already picks up new test names from the `tests` output; no
   change needed for T0-T8 beyond the rename.
2. Add a crash check after the loop. Every F1 shape was a `SIGSEGV`, and a
   `command_ok` failure alone does not distinguish "wrong error" from "backend
   died and the cluster recovered":

```perl
ok(!$node->log_contains(qr/was terminated by signal/, $log_start),
	'no backend crashed during the cursor-option tests');
```

   with `my $log_start = -s $node->logfile;` captured before the loop.

3. Resolve the `max_protocol_version=latest` question from file 02 §D1: either
   document the requirement or drop it from the connection string here
   (`:29`).

---

## Build / verify (this commit)

```sh
ninja -C build18new
ninja -C build18new install
meson test -C build18new libpq_protocol_cursor / 001_libpq_protocol_cursor -v
```

Then confirm the tests actually catch the bugs — a regression test that passes
against the broken code is worthless. Stash commit 1's backend change, rebuild,
and check that each new case fails as expected:

```sh
git stash push src/backend/tcop/postgres.c
ninja -C build18new && ninja -C build18new install
meson test -C build18new 001_libpq_protocol_cursor -v   # must FAIL
git stash pop
```

Expected failures against the unfixed backend: T1 (backend killed by signal 11,
`log_contains` check trips), T2, T3 (`GROUP BY`, `DISTINCT`, `SELECT 1` row
mismatch versus the reference cursor), T4 (backward fetch wrongly succeeds), T6,
T7 (step 1 wrongly succeeds). T5 and T8 fail against the unfixed libpq instead.

Run the module under a **non-assert** build as well as the normal `cassert`
build: two of the three F1 crash shapes trip an Assert before reaching the NULL
dereference, so an assert-enabled build reports a different failure than the
production one.

---

## Definition of done

- [ ] `dml_with_cursor_options` renamed and inverted; no test asserts that a
      cursor option is silently ignored.
- [ ] All four non-`SELECT` portal strategies covered for `HOLD` and for
      `SCROLL`, each followed by a `COMMIT` that the connection survives.
- [ ] Differential `SCROLL` test covers at minimum HashAgg, `DISTINCT`,
      `Result` (`SELECT 1`), `Append` and a plain seqscan, and compares against
      `DECLARE ... SCROLL CURSOR` rather than against hardcoded values.
- [ ] `HOLD`-only portal refuses backward fetch before and after commit.
- [ ] Locking-clause rejection asserted on errdetail.
- [ ] Plan-cache non-poisoning asserted across the five-custom-plan threshold.
- [ ] Client-side `nParams` and `SCROLL｜NO_SCROLL` rejections asserted,
      including that the connection stays usable.
- [ ] TAP driver fails if any backend was terminated by a signal.
- [ ] Every new case verified to fail against the pre-fix tree.
- [ ] Suite green on both `cassert=true` and `cassert=false` builds.
