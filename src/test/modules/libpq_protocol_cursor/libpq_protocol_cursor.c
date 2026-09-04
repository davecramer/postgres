/*-------------------------------------------------------------------------
 *
 * libpq_protocol_cursor.c
 *		Tests for extended query protocol cursor options via
 *		PQsendBindWithCursorOptions (_pq_.protocol_cursor protocol extension).
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/libpq_protocol_cursor/libpq_protocol_cursor.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <string.h>

#include "libpq-fe.h"
#include "pg_getopt.h"

/*
 * Cursor option flags for PQsendBindWithCursorOptions, defined in libpq-fe.h
 * as PQ_BIND_CURSOR_*.  We use those directly.
 */

static const char *const progname = "libpq_protocol_cursor";

static void exit_nicely(PGconn *conn);
pg_noreturn static void pg_fatal_impl(int line, const char *fmt, ...)
			pg_attribute_printf(2, 3);

static void
exit_nicely(PGconn *conn)
{
	PQfinish(conn);
	exit(1);
}

/*
 * The following few functions are wrapped in macros to make the reported line
 * number in an error match the line number of the invocation.
 */

/*
 * Print an error to stderr and terminate the program.
 */
#define pg_fatal(...) pg_fatal_impl(__LINE__, __VA_ARGS__)
pg_noreturn static void
pg_fatal_impl(int line, const char *fmt, ...)
{
	va_list		args;

	fflush(stdout);

	fprintf(stderr, "\n%s:%d: ", progname, line);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	Assert(fmt[strlen(fmt) - 1] != '\n');
	fprintf(stderr, "\n");
	exit(1);
}

/*
 * Check that libpq next returns a PGresult with the specified status,
 * returning the PGresult so that caller can perform additional checks.
 */
#define confirm_result_status(conn, status) confirm_result_status_impl(__LINE__, conn, status)
static PGresult *
confirm_result_status_impl(int line, PGconn *conn, ExecStatusType status)
{
	PGresult   *res;

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal_impl(line, "PQgetResult returned null unexpectedly: %s",
					  PQerrorMessage(conn));
	if (PQresultStatus(res) != status)
		pg_fatal_impl(line, "PQgetResult returned status %s, expected %s: %s",
					  PQresStatus(PQresultStatus(res)),
					  PQresStatus(status),
					  PQerrorMessage(conn));
	return res;
}

/*
 * Check that libpq next returns a PGresult with the specified status,
 * then free the PGresult.
 */
#define consume_result_status(conn, status) consume_result_status_impl(__LINE__, conn, status)
static void
consume_result_status_impl(int line, PGconn *conn, ExecStatusType status)
{
	PGresult   *res;

	res = confirm_result_status_impl(line, conn, status);
	PQclear(res);
}

/*
 * Check that libpq next returns a null PGresult.
 */
#define consume_null_result(conn) consume_null_result_impl(__LINE__, conn)
static void
consume_null_result_impl(int line, PGconn *conn)
{
	PGresult   *res;

	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal_impl(line, "expected NULL PGresult, got %s: %s",
					  PQresStatus(PQresultStatus(res)),
					  PQerrorMessage(conn));
}

/*
 * Run a command with PQexec, failing if it does not return the expected
 * status.  Returns the PGresult so the caller can inspect it.
 */
#define exec_expect(conn, sql, status) exec_expect_impl(__LINE__, conn, sql, status)
static PGresult *
exec_expect_impl(int line, PGconn *conn, const char *sql, ExecStatusType status)
{
	PGresult   *res = PQexec(conn, sql);

	if (PQresultStatus(res) != status)
		pg_fatal_impl(line, "\"%s\" returned status %s, expected %s: %s",
					  sql, PQresStatus(PQresultStatus(res)),
					  PQresStatus(status), PQresultErrorMessage(res));
	return res;
}

/*
 * Run a command with PQexec, failing unless it returns PGRES_COMMAND_OK.
 */
#define exec_ok(conn, sql) \
	PQclear(exec_expect_impl(__LINE__, conn, sql, PGRES_COMMAND_OK))

/*
 * Run a command with PQexec and require it to fail with the given SQLSTATE,
 * and with the given primary message if that is not NULL.
 */
#define exec_expect_error(conn, sql, sqlstate, msg) \
	exec_expect_error_impl(__LINE__, conn, sql, sqlstate, msg)
static void
exec_expect_error_impl(int line, PGconn *conn, const char *sql,
					   const char *sqlstate, const char *msg)
{
	PGresult   *res = PQexec(conn, sql);
	const char *val;

	if (PQresultStatus(res) != PGRES_FATAL_ERROR)
		pg_fatal_impl(line, "\"%s\" returned status %s, expected an error",
					  sql, PQresStatus(PQresultStatus(res)));

	val = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	if (val == NULL || strcmp(val, sqlstate) != 0)
		pg_fatal_impl(line, "\"%s\": expected SQLSTATE %s, got %s",
					  sql, sqlstate, val ? val : "(none)");

	val = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	if (msg != NULL && (val == NULL || strcmp(val, msg) != 0))
		pg_fatal_impl(line, "\"%s\": expected message \"%s\", got \"%s\"",
					  sql, msg, val ? val : "(none)");
	PQclear(res);
}

/*
 * Create a temporary table with three rows.
 */
static void
setup_table(PGconn *conn, const char *name)
{
	char		sql[128];

	snprintf(sql, sizeof(sql), "CREATE TEMP TABLE %s(id int)", name);
	exec_ok(conn, sql);
	snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (1), (2), (3)", name);
	exec_ok(conn, sql);
}

/*
 * Send Bind+Describe with the given cursor options and consume the results,
 * expecting the portal to be created successfully.
 */
#define bind_cursor_ok(conn, stmt, portal, flags) \
	bind_cursor_ok_impl(__LINE__, conn, stmt, portal, flags)
static void
bind_cursor_ok_impl(int line, PGconn *conn, const char *stmt,
					const char *portal, int flags)
{
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal_impl(line, "failed to enter pipeline mode: %s",
					  PQerrorMessage(conn));

	if (PQsendBindWithCursorOptions(conn, stmt, 0, NULL, NULL, NULL, 0,
									portal, flags) != 1)
		pg_fatal_impl(line, "PQsendBindWithCursorOptions failed: %s",
					  PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal_impl(line, "pipeline sync failed: %s", PQerrorMessage(conn));

	consume_result_status_impl(line, conn, PGRES_COMMAND_OK);
	consume_null_result_impl(line, conn);
	consume_result_status_impl(line, conn, PGRES_PIPELINE_SYNC);
	consume_null_result_impl(line, conn);

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal_impl(line, "failed to exit pipeline mode: %s",
					  PQerrorMessage(conn));
}

/*
 * Same, but require the server to reject the Bind with the given SQLSTATE,
 * and with the given error detail if that is not NULL.
 */
#define bind_cursor_error(conn, stmt, portal, flags, sqlstate, detail) \
	bind_cursor_error_impl(__LINE__, conn, stmt, portal, flags, sqlstate, detail)
static void
bind_cursor_error_impl(int line, PGconn *conn, const char *stmt,
					   const char *portal, int flags,
					   const char *sqlstate, const char *detail)
{
	PGresult   *res;
	const char *val;

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal_impl(line, "failed to enter pipeline mode: %s",
					  PQerrorMessage(conn));

	if (PQsendBindWithCursorOptions(conn, stmt, 0, NULL, NULL, NULL, 0,
									portal, flags) != 1)
		pg_fatal_impl(line, "PQsendBindWithCursorOptions failed: %s",
					  PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal_impl(line, "pipeline sync failed: %s", PQerrorMessage(conn));

	res = confirm_result_status_impl(line, conn, PGRES_FATAL_ERROR);

	val = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	if (val == NULL || strcmp(val, sqlstate) != 0)
		pg_fatal_impl(line, "expected SQLSTATE %s, got %s",
					  sqlstate, val ? val : "(none)");

	val = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	if (detail != NULL && (val == NULL || strcmp(val, detail) != 0))
		pg_fatal_impl(line, "expected errdetail \"%s\", got \"%s\"",
					  detail, val ? val : "(none)");
	PQclear(res);
	consume_null_result_impl(line, conn);

	consume_result_status_impl(line, conn, PGRES_PIPELINE_SYNC);
	consume_null_result_impl(line, conn);

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal_impl(line, "failed to exit pipeline mode: %s",
					  PQerrorMessage(conn));
}

/*
 * Prepare a statement, failing on error.
 */
#define prepare_ok(conn, name, query) prepare_ok_impl(__LINE__, conn, name, query)
static void
prepare_ok_impl(int line, PGconn *conn, const char *name, const char *query)
{
	PGresult   *res = PQprepare(conn, name, query, 0, NULL);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal_impl(line, "PREPARE of \"%s\" failed: %s", query,
					  PQresultErrorMessage(res));
	PQclear(res);
}

/*
 * Test holdable cursor: create a portal with PQ_BIND_CURSOR_HOLD via Bind,
 * commit the transaction, then FETCH from the surviving portal.
 */
static void
test_holdable_cursor(PGconn *conn)
{
	PGresult   *res;

	fprintf(stderr, "test_holdable_cursor... ");

	res = PQexec(conn, "BEGIN");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("BEGIN failed: %s", PQerrorMessage(conn));
	PQclear(res);

	res = PQexec(conn, "CREATE TEMP TABLE IF NOT EXISTS holdable_test(id int)");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("CREATE TABLE failed: %s", PQerrorMessage(conn));
	PQclear(res);

	res = PQexec(conn, "INSERT INTO holdable_test VALUES (1), (2), (3)");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("INSERT failed: %s", PQerrorMessage(conn));
	PQclear(res);

	res = PQprepare(conn, "holdstmt", "SELECT * FROM holdable_test", 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("PREPARE failed: %s", PQerrorMessage(conn));
	PQclear(res);

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	if (PQsendBindWithCursorOptions(conn, "holdstmt", 0, NULL, NULL, NULL, 0,
									"holdportal", PQ_BIND_CURSOR_HOLD) != 1)
		pg_fatal("PQsendBindWithCursorOptions failed: %s", PQerrorMessage(conn));

	if (PQsendQueryParams(conn, "COMMIT", 0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("COMMIT failed: %s", PQerrorMessage(conn));

	if (PQsendQueryParams(conn, "FETCH ALL FROM holdportal", 0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("FETCH failed: %s", PQerrorMessage(conn));

	if (PQsendClosePortal(conn, "holdportal") != 1)
		pg_fatal("PQsendClosePortal failed: %s", PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	/* Bind+Describe result (RowDescription metadata) */
	res = confirm_result_status(conn, PGRES_COMMAND_OK);
	if (PQnfields(res) != 1)
		pg_fatal("expected 1 field, got %d", PQnfields(res));
	PQclear(res);
	consume_null_result(conn);

	/* COMMIT result */
	consume_result_status(conn, PGRES_COMMAND_OK);
	consume_null_result(conn);

	/* FETCH after commit */
	res = confirm_result_status(conn, PGRES_TUPLES_OK);
	if (PQntuples(res) != 3)
		pg_fatal("expected 3 rows after commit, got %d", PQntuples(res));
	PQclear(res);
	consume_null_result(conn);

	/* CLOSE */
	consume_result_status(conn, PGRES_COMMAND_OK);
	consume_null_result(conn);

	consume_result_status(conn, PGRES_PIPELINE_SYNC);
	consume_null_result(conn);

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to exit pipeline mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

/*
 * Test that cursor options on a DML statement are rejected.  Such a portal is
 * not PORTAL_ONE_SELECT, so the options cannot be honored; the server must say
 * so rather than accept them and ignore them.
 */
static void
test_dml_cursor_options_rejected(PGconn *conn)
{
	PGresult   *res;

	fprintf(stderr, "test_dml_cursor_options_rejected... ");

	exec_ok(conn, "CREATE TEMP TABLE dml_test(id int)");
	prepare_ok(conn, "dmlstmt", "INSERT INTO dml_test VALUES (1), (2), (3)");

	/* HOLD on a DML statement cannot be honored and must be rejected */
	bind_cursor_error(conn, "dmlstmt", "dmlportal", PQ_BIND_CURSOR_HOLD,
					  "0A000",
					  "Holdable portals are supported only for a single SELECT statement.");

	/* Verify the INSERT didn't execute */
	res = exec_expect(conn, "SELECT count(*) FROM dml_test", PGRES_TUPLES_OK);
	if (strcmp(PQgetvalue(res, 0, 0), "0") != 0)
		pg_fatal("expected 0 rows (the Bind was rejected), got %s",
				 PQgetvalue(res, 0, 0));
	PQclear(res);

	fprintf(stderr, "ok\n");
}

/*
 * Test client-side validation: PQsendBindWithCursorOptions should reject
 * an unnamed (empty) portal.
 */
static void
test_unnamed_portal_rejected(PGconn *conn)
{
	PGresult   *res;

	fprintf(stderr, "test_unnamed_portal_rejected... ");

	res = PQprepare(conn, "rejectstmt", "SELECT 1", 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("PREPARE failed: %s", PQerrorMessage(conn));
	PQclear(res);

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	/* Empty portal name should be rejected client-side */
	if (PQsendBindWithCursorOptions(conn, "rejectstmt", 0, NULL, NULL, NULL, 0,
									"", PQ_BIND_CURSOR_HOLD) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject empty portal name");

	/* NULL portal name should also be rejected */
	if (PQsendBindWithCursorOptions(conn, "rejectstmt", 0, NULL, NULL, NULL, 0,
									NULL, PQ_BIND_CURSOR_HOLD) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject NULL portal name");

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to exit pipeline mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

/*
 * Test that cursor options are rejected when _pq_.protocol_cursor is not negotiated.
 * HOLD is requested but the extension is disabled, so the API call itself
 * returns 0.
 */
static void
test_cursor_options_without_extension(PGconn *conn)
{
	PGresult   *res;

	fprintf(stderr, "test_cursor_options_without_extension... ");

	/*
	 * PQPortalCursorEnabled should return false when extension is not
	 * negotiated
	 */
	if (PQPortalCursorEnabled(conn) != 0)
		pg_fatal("expected PQPortalCursorEnabled to return false");

	res = PQprepare(conn, "noextstmt", "SELECT 1", 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("PREPARE failed: %s", PQerrorMessage(conn));
	PQclear(res);

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	/* Non-zero cursorOptions should be rejected when extension is disabled */
	if (PQsendBindWithCursorOptions(conn, "noextstmt", 0, NULL, NULL, NULL, 0,
									"noextportal", PQ_BIND_CURSOR_HOLD) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject cursor options");

	/* Zero cursorOptions should still succeed */
	if (PQsendBindWithCursorOptions(conn, "noextstmt", 0, NULL, NULL, NULL, 0,
									"noextportal", 0) != 1)
		pg_fatal("PQsendBindWithCursorOptions with zero options failed: %s",
				 PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	/* Bind+Describe result */
	res = confirm_result_status(conn, PGRES_COMMAND_OK);
	PQclear(res);
	consume_null_result(conn);

	consume_result_status(conn, PGRES_PIPELINE_SYNC);
	consume_null_result(conn);

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to exit pipeline mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

static void
usage(const char *progname)
{
	fprintf(stderr, "%s tests extended query protocol cursor options.\n\n", progname);
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s tests\n", progname);
	fprintf(stderr, "  %s TESTNAME [CONNINFO]\n", progname);
}

/*
 * Test that invalid cursor option flags are rejected client-side.
 */
static void
test_invalid_flags_rejected(PGconn *conn)
{
	PGresult   *res;

	fprintf(stderr, "test_invalid_flags_rejected... ");

	res = PQprepare(conn, "invalidstmt", "SELECT 1", 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("PREPARE failed: %s", PQerrorMessage(conn));
	PQclear(res);

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	/* Flag 0x0002 is not a valid bind extension flag */
	if (PQsendBindWithCursorOptions(conn, "invalidstmt", 0, NULL, NULL, NULL, 0,
									"invalidportal", 0x0002) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject invalid flags");

	/* Combination of valid and invalid flags should also be rejected */
	if (PQsendBindWithCursorOptions(conn, "invalidstmt", 0, NULL, NULL, NULL, 0,
									"invalidportal",
									PQ_BIND_CURSOR_HOLD | 0x0100) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject mixed invalid flags");

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to exit pipeline mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

/*
 * Statements whose portals are not PORTAL_ONE_SELECT, one per strategy.
 * Cursor options cannot be honored for any of them.
 */
static const char *const non_select_statements[] = {
	/* PORTAL_ONE_RETURNING */
	"INSERT INTO nonselect_test VALUES (9) RETURNING id",
	/* PORTAL_MULTI_QUERY */
	"INSERT INTO nonselect_test VALUES (9)",
	/* PORTAL_UTIL_SELECT */
	"SHOW work_mem",
	/* PORTAL_ONE_MOD_WITH */
	"WITH c AS (INSERT INTO nonselect_test VALUES (9) RETURNING id) SELECT * FROM c"
};

/*
 * Common body of the test below: request the given cursor options for a portal
 * over each statement above and require the server to reject the Bind, both
 * when the transaction is then rolled back and when it is committed.
 *
 * The commit is the point of the test.  A holdable portal over one of these
 * statements used to reach PreCommit_Portals() with no QueryDesc and take the
 * backend down with it.
 */
static void
cursor_options_on_non_select(PGconn *conn, int flags)
{
	PGresult   *res;

	setup_table(conn, "nonselect_test");

	for (int i = 0; i < lengthof(non_select_statements); i++)
	{
		char		stmtname[32];
		char		portalname[32];

		snprintf(stmtname, sizeof(stmtname), "nsstmt%d", i);
		snprintf(portalname, sizeof(portalname), "nsportal%d", i);
		prepare_ok(conn, stmtname, non_select_statements[i]);

		/* First, roll back after the rejected Bind */
		exec_ok(conn, "BEGIN");
		bind_cursor_error(conn, stmtname, portalname, flags, "0A000",
						  "Holdable portals are supported only for a single SELECT statement.");
		exec_ok(conn, "ROLLBACK");

		/* Then, commit after it: the sequence that used to crash */
		exec_ok(conn, "BEGIN");
		bind_cursor_error(conn, stmtname, portalname, flags, "0A000",
						  "Holdable portals are supported only for a single SELECT statement.");
		exec_ok(conn, "COMMIT");

		if (PQstatus(conn) != CONNECTION_OK)
			pg_fatal("connection lost at COMMIT after Bind on \"%s\"",
					 non_select_statements[i]);

		PQclear(exec_expect(conn, "SELECT 1", PGRES_TUPLES_OK));
	}

	/* None of those statements should have executed */
	res = exec_expect(conn, "SELECT count(*) FROM nonselect_test",
					  PGRES_TUPLES_OK);
	if (strcmp(PQgetvalue(res, 0, 0), "3") != 0)
		pg_fatal("expected the table to be unchanged (3 rows), found %s",
				 PQgetvalue(res, 0, 0));
	PQclear(res);
}

/*
 * Test that PQ_BIND_CURSOR_HOLD is rejected for portals that are not a single
 * SELECT, and that committing afterwards does not take the backend down.
 */
static void
test_hold_on_non_select_rejected(PGconn *conn)
{
	fprintf(stderr, "test_hold_on_non_select_rejected... ");
	cursor_options_on_non_select(conn, PQ_BIND_CURSOR_HOLD);
	fprintf(stderr, "ok\n");
}

/*
 * Test that PQ_BIND_CURSOR_HOLD does not make a portal scrollable: the portal
 * keeps the NO_SCROLL default a DECLARE CURSOR without SCROLL has, both inside
 * the transaction and after the commit that persists it.
 */
static void
test_hold_only_keeps_no_scroll(PGconn *conn)
{
	PGresult   *res;

	fprintf(stderr, "test_hold_only_keeps_no_scroll... ");

	setup_table(conn, "holdonly_test");
	prepare_ok(conn, "holdonlystmt", "SELECT id FROM holdonly_test");

	/* Before the commit: backward fetch is refused */
	exec_ok(conn, "BEGIN");
	bind_cursor_ok(conn, "holdonlystmt", "holdonlyportal1",
				   PQ_BIND_CURSOR_HOLD);

	res = exec_expect(conn, "FETCH 2 FROM holdonlyportal1", PGRES_TUPLES_OK);
	if (PQntuples(res) != 2)
		pg_fatal("expected 2 rows from forward fetch, got %d", PQntuples(res));
	PQclear(res);

	exec_expect_error(conn, "FETCH BACKWARD 1 FROM holdonlyportal1", "55000",
					  "cursor can only scan forward");
	exec_ok(conn, "ROLLBACK");

	/* After the commit: still usable forwards, still not backwards */
	exec_ok(conn, "BEGIN");
	bind_cursor_ok(conn, "holdonlystmt", "holdonlyportal2",
				   PQ_BIND_CURSOR_HOLD);

	res = exec_expect(conn, "FETCH 2 FROM holdonlyportal2", PGRES_TUPLES_OK);
	if (PQntuples(res) != 2)
		pg_fatal("expected 2 rows from forward fetch, got %d", PQntuples(res));
	PQclear(res);

	exec_ok(conn, "COMMIT");

	res = exec_expect(conn, "FETCH 1 FROM holdonlyportal2", PGRES_TUPLES_OK);
	if (PQntuples(res) != 1)
		pg_fatal("expected 1 row from the held portal, got %d", PQntuples(res));
	if (strcmp(PQgetvalue(res, 0, 0), "3") != 0)
		pg_fatal("expected value '3' after commit, got '%s'",
				 PQgetvalue(res, 0, 0));
	PQclear(res);

	/* The hold store was built without random access, so this still fails */
	exec_expect_error(conn, "FETCH BACKWARD 1 FROM holdonlyportal2", "55000",
					  "cursor can only scan forward");

	fprintf(stderr, "ok\n");
}

/*
 * Test that a query with a row locking clause is rejected for HOLD, with the
 * same errdetail DECLARE CURSOR uses.
 */
static void
test_locking_clause_rejected(PGconn *conn)
{
	fprintf(stderr, "test_locking_clause_rejected... ");

	setup_table(conn, "locking_test");
	prepare_ok(conn, "lockstmt", "SELECT id FROM locking_test FOR UPDATE");

	exec_ok(conn, "BEGIN");
	bind_cursor_error(conn, "lockstmt", "lockportal", PQ_BIND_CURSOR_HOLD,
					  "0A000", "Holdable cursors must be READ ONLY.");
	exec_ok(conn, "ROLLBACK");

	fprintf(stderr, "ok\n");
}

/*
 * Test that an out-of-range parameter count is rejected client-side, so that
 * no malformed Bind reaches the wire.
 */
static void
test_bind_param_limits(PGconn *conn)
{
	fprintf(stderr, "test_bind_param_limits... ");

	prepare_ok(conn, "limitstmt", "SELECT 1");

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	if (PQsendBindWithCursorOptions(conn, "limitstmt", -1, NULL, NULL, NULL, 0,
									"limitportal", 0) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject nParams = -1");
	if (strstr(PQerrorMessage(conn),
			   "number of parameters must be between 0 and 65535") == NULL)
		pg_fatal("unexpected error message: %s", PQerrorMessage(conn));

	if (PQsendBindWithCursorOptions(conn, "limitstmt", 65536, NULL, NULL, NULL,
									0, "limitportal", 0) != 0)
		pg_fatal("expected PQsendBindWithCursorOptions to reject nParams = 65536");
	if (strstr(PQerrorMessage(conn),
			   "number of parameters must be between 0 and 65535") == NULL)
		pg_fatal("unexpected error message: %s", PQerrorMessage(conn));

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to exit pipeline mode: %s", PQerrorMessage(conn));

	/* Nothing was sent, so the connection is still in sync */
	if (PQstatus(conn) != CONNECTION_OK)
		pg_fatal("connection lost");
	PQclear(exec_expect(conn, "SELECT 1", PGRES_TUPLES_OK));

	fprintf(stderr, "ok\n");
}

static void
print_test_list(void)
{
	printf("holdable_cursor\n");
	printf("hold_only_keeps_no_scroll\n");
	printf("dml_cursor_options_rejected\n");
	printf("hold_on_non_select_rejected\n");
	printf("locking_clause_rejected\n");
	printf("unnamed_portal_rejected\n");
	printf("invalid_flags_rejected\n");
	printf("bind_param_limits\n");
	printf("cursor_options_without_extension\n");
}

int
main(int argc, char **argv)
{
	const char *conninfo = "";
	PGconn	   *conn;
	char	   *testname;
	PGresult   *res;

	if (argc < 2)
	{
		usage(argv[0]);
		exit(1);
	}

	testname = argv[1];

	if (strcmp(testname, "tests") == 0)
	{
		print_test_list();
		exit(0);
	}

	if (argc > 2)
		conninfo = argv[2];

	conn = PQconnectdb(conninfo);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s\n",
				PQerrorMessage(conn));
		exit_nicely(conn);
	}

	res = PQexec(conn, "SET lc_messages TO \"C\"");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to set \"lc_messages\": %s", PQerrorMessage(conn));
	PQclear(res);

	if (strcmp(testname, "bind_param_limits") == 0)
		test_bind_param_limits(conn);
	else if (strcmp(testname, "cursor_options_without_extension") == 0)
		test_cursor_options_without_extension(conn);
	else if (strcmp(testname, "dml_cursor_options_rejected") == 0)
		test_dml_cursor_options_rejected(conn);
	else if (strcmp(testname, "hold_on_non_select_rejected") == 0)
		test_hold_on_non_select_rejected(conn);
	else if (strcmp(testname, "hold_only_keeps_no_scroll") == 0)
		test_hold_only_keeps_no_scroll(conn);
	else if (strcmp(testname, "holdable_cursor") == 0)
		test_holdable_cursor(conn);
	else if (strcmp(testname, "invalid_flags_rejected") == 0)
		test_invalid_flags_rejected(conn);
	else if (strcmp(testname, "locking_clause_rejected") == 0)
		test_locking_clause_rejected(conn);
	else if (strcmp(testname, "unnamed_portal_rejected") == 0)
		test_unnamed_portal_rejected(conn);
	else
	{
		fprintf(stderr, "\"%s\" is not a recognized test name\n", testname);
		exit(1);
	}

	PQfinish(conn);
	return 0;
}
