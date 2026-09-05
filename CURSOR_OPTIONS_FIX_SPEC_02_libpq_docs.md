# Spec — Commit 2/3: libpq and documentation

Fixes F4 (client half), F5 and F6 from `CURSOR_OPTIONS_FIX_SPEC.md`, and the
documentation defects listed there. Depends on commit 1 only for the error
messages quoted in the docs; the code changes here are independent.

**Scope of this commit:** `src/interfaces/libpq/fe-exec.c`,
`src/interfaces/libpq/fe-connect.c`, `src/interfaces/libpq/libpq-fe.h`,
`doc/src/sgml/libpq.sgml`, `doc/src/sgml/protocol.sgml`. No new exported
symbols, so `exports.txt` is untouched. No wire-format change.

Line numbers are anchors against the current working tree, not literals.

---

## C1. `nParams` range check (F5)

`PQsendBindWithCursorOptions` (`fe-exec.c:1696`) writes the parameter count with
`pqPutInt(nParams, 2, conn)` (`:1749`, `:1758`) but never bounds `nParams`. Its
three siblings all do: `PQsendQueryParams` (`:1527`), `PQsendPrepare` (`:1573`),
`PQsendQueryPrepared` (`:1667`). With `nParams = 65536` the count truncates to
`0` while the loop at `:1761` still appends 65536 parameter values, so the server
reads the first four bytes of the first parameter as the result-format count and
the connection desynchronizes; a negative `nParams` walks the caller's arrays.

Add the check with the other argument checks, immediately after the `stmtName`
NULL test at `:1711-1715`:

```c
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}
```

Reuse the existing message verbatim so no new translatable string is added.

While here, drop the redundant `portalName ? portalName : ""` at `:1743`: the
function has already rejected a NULL `portalName` at `:1717`.

---

## C2. Reject `SCROLL｜NO_SCROLL` client-side (F4, client half)

Commit 1 rejects the combination server-side. libpq should not spend a round
trip on a request that cannot succeed, and the failure is easier to attribute
locally. Extend the flag validation at `fe-exec.c:1730`:

```c
	if (cursorOptions & ~PQ_BIND_CURSOR_VALID_FLAGS)
	{
		libpq_append_conn_error(conn,
								"unrecognized cursor option flags: 0x%x",
								cursorOptions & ~PQ_BIND_CURSOR_VALID_FLAGS);
		return 0;
	}

	if ((cursorOptions & PQ_BIND_CURSOR_SCROLL) &&
		(cursorOptions & PQ_BIND_CURSOR_NO_SCROLL))
	{
		libpq_append_conn_error(conn,
								"cannot specify both PQ_BIND_CURSOR_SCROLL and PQ_BIND_CURSOR_NO_SCROLL");
		return 0;
	}
```

Also remove the compatibility alias `PQ_BIND_EXT_VALID_FLAGS`
(`libpq-fe.h:87`). The extension has never been released, so nothing can depend
on the older spelling, and leaving two names for one bitmask invites drift.

---

## C3. Free `conn->protocol_cursor` (F6)

`protocol_cursor` is an ordinary conninfo string option (`fe-connect.c:423-425`)
and is `strdup`'d into `conn` like the rest, but `freePGconn` never releases it.
Add it to the free list at `fe-connect.c:5140`, keeping the list's existing
grouping:

```c
	free(conn->scram_client_key);
	free(conn->scram_server_key);
	free(conn->sslkeylogfile);
	free(conn->protocol_cursor);
	free(conn->oauth_issuer);
```

> One `strdup`'d string per `PGconn`, so this only matters for programs that
> open and close many connections, but every other conninfo field is freed here
> and an omission in this list is exactly the kind of thing that is never found
> later.

---

## D1. `libpq.sgml`: document the `protocol_cursor` connection parameter

The parameter is currently referenced only from the two new function entries
(`libpq.sgml:3146`, `:5580`, `:5593`, `:5596`) and appears nowhere in
§34.1.2 "Parameter Key Words", so there is no way for a reader to discover it.
Add a `<varlistentry>` after the `max_protocol_version` entry
(`libpq.sgml:2211`, ends `:2235`, immediately before
`libpq-connect-krbsrvname`), which is where the other protocol-negotiation
parameters live:

```sgml
     <varlistentry id="libpq-connect-protocol_cursor" xreflabel="protocol_cursor">
      <term><literal>protocol_cursor</literal></term>
      <listitem>
       <para>
        Requests the <literal>_pq_.protocol_cursor</literal> protocol extension,
        which allows cursor options to be attached to a named portal with
        <xref linkend="libpq-PQsendBindWithCursorOptions"/>.  Set this to
        <literal>1</literal> to request the extension; the default is
        <literal>0</literal>.
       </para>

       <para>
        The extension is only usable if the server also supports it.  If the
        server rejects it, the connection still succeeds and
        <xref linkend="libpq-PQPortalCursorEnabled"/> returns false; calls to
        <xref linkend="libpq-PQsendBindWithCursorOptions"/> with non-zero cursor
        options then fail without contacting the server.
       </para>
      </listitem>
     </varlistentry>
```

> Open item to settle while implementing: the TAP driver appends
> `max_protocol_version=latest` alongside `protocol_cursor=1`
> (`t/001_libpq_protocol_cursor.pl:29`). Determine whether that is actually
> required — `build_startup_packet` adds the `_pq_.` option unconditionally
> (`fe-protocol3.c:2518-2520`), and `NegotiateProtocolVersion` exists in 3.0 —
> and if it is required, say so in this entry and make `protocol_cursor=1` raise
> the requested version the way other version-dependent options do. If it is not
> required, drop it from the TAP driver in commit 3.

## D2. `libpq.sgml`: document the new failure modes

In the `PQsendBindWithCursorOptions` entry (`libpq.sgml:5556`), state the
constraints commit 1 introduces, so the documented contract matches the code:

- `PQ_BIND_CURSOR_SCROLL` and `PQ_BIND_CURSOR_NO_SCROLL` are mutually exclusive.
- `PQ_BIND_CURSOR_SCROLL` and `PQ_BIND_CURSOR_HOLD` require the portal's
  statement to be a single `SELECT` without a row-locking clause; other
  statements draw an error from the server rather than silently ignoring the
  options.
- A portal created with `PQ_BIND_CURSOR_SCROLL` may be fetched backwards; one
  created without it may not, exactly as for `DECLARE ... SCROLL CURSOR`.
- `nParams` must be between 0 and 65535.

## D3. `protocol.sgml`: attribute the Bind bitmap to its extension

The Bind message's new field (`protocol.sgml:4421-4428`) reads:

```sgml
      <varlistentry>
       <term>Int32 (optional)</term>
       <listitem>
        <para>
         Bitmap set by protocol extensions.
        </para>
       </listitem>
      </varlistentry>
```

A reader cannot decode a Bind message from that, and a second extension adding
its own trailing field would make the message ambiguous. Name the owning
extension and its flags:

```sgml
      <varlistentry>
       <term>Int32 (optional)</term>
       <listitem>
        <para>
         Cursor options for the portal, present only when the
         <literal>_pq_.protocol_cursor</literal> protocol extension has been
         negotiated.  The value is a bitmap:
         <literal>1</literal> (<literal>SCROLL</literal>),
         <literal>2</literal> (<literal>NO SCROLL</literal>) and
         <literal>4</literal> (<literal>WITH HOLD</literal>); other bits are
         reserved and must be zero.  <literal>SCROLL</literal> and
         <literal>NO SCROLL</literal> must not both be set.  A value of
         <literal>0</literal> is equivalent to omitting the field.
        </para>
        <para>
         <literal>SCROLL</literal> and <literal>WITH HOLD</literal> require the
         prepared statement to be a single <command>SELECT</command> without a
         row-locking clause, the same restrictions
         <xref linkend="sql-declare"/> imposes.
        </para>
       </listitem>
      </varlistentry>
```

## D4. `protocol.sgml`: correct the fetch-count sentinel

The Execute-trailer documentation (uncommitted, `protocol.sgml:5215-5225`)
claims the largest positive 64-bit integer is "reserved as a sentinel meaning
*fetch all rows* (equivalent to `FETCH ALL`)" without qualification. Measured
behavior contradicts that for two of the four directions: `fetch_count_wire_to_long`
maps it to `FETCH_ALL` (`postgres.c:2196`), and `DoPortalRunFetch`
(`pquery.c:1471`) only reads `FETCH_ALL` as "all remaining" for
`FETCH_FORWARD`/`FETCH_BACKWARD`. With `ABSOLUTE` or `RELATIVE` it is a literal
row position, which lands past the end of any real result and returns zero rows.

```sgml
         The fetch count, interpreted according to the fetch direction,
         following the semantics of the SQL <command>FETCH</command>
         command.  This is a signed 64-bit integer.
         With <literal>FORWARD</literal> or <literal>BACKWARD</literal>,
         <productname>PostgreSQL</productname>'s largest positive 64-bit
         integer value is reserved as a sentinel meaning
         <quote>all remaining rows in that direction</quote> (equivalent to
         <command>FETCH ALL</command> and <command>FETCH BACKWARD ALL</command>);
         it is a portable spelling of that request, since the equivalent
         internal value is platform dependent.  With
         <literal>ABSOLUTE</literal> or <literal>RELATIVE</literal> the value is
         a literal row position or offset, and this sentinel has no special
         meaning: it addresses a position past the end of any result set and so
         returns no rows.  A count of <literal>0</literal> is a zero-row no-op,
         equivalent to <command>FETCH FORWARD 0</command>.
```

Also state, in the paragraph introducing the trailer, that a backward or
absolute fetch requires the portal to have been created with the
`SCROLL` cursor option — otherwise the server reports
`cursor can only scan forward` (`pquery.c:933`), which is not currently
discoverable from the Execute documentation.

## D5. `protocol.sgml`: version and description of the extension

In the supported-extensions table (`protocol.sgml:336-357`):

- "PostgreSQL 19 and later" is wrong against a 20devel tree; the extension has
  not shipped. Use the release this actually lands in.
- The description names the C constants `PQ_BIND_CURSOR_*` from `libpq-fe.h`.
  `protocol.sgml` documents the wire protocol for all clients, not just libpq;
  give the numeric values (as in §D3) and leave the libpq constant names to
  `libpq.sgml`.

## D6. Naming drift in the commit message

Commit 96842bb's message says `_pq_.cursor_bind` in the subject and
`cursor_protocol` for the connection parameter; the code and docs say
`_pq_.protocol_cursor` and `protocol_cursor` throughout. Rewrite the message
when the series is rebased — a wrong extension name in the commit that
introduces it is the first thing anyone bisecting will search for. `AmazonQ.md`,
`BIND_HOLD_SUMMARY.md`, `PQSENDBIND_ADDITION.md` and
`PROTOCOL_OPTIONS_MIGRATION.md` in the working tree carry the same stale names;
they are untracked scratch files and are not part of the series, but should not
be committed as-is.

---

## Build / verify (this commit)

```sh
ninja -C build18new
ninja -C build18new docs               # or: ninja -C build18new html
meson test -C build18new --suite regress
meson test -C build18new libpq_protocol_cursor / 001_libpq_protocol_cursor
```

The SGML must build clean — check for unresolved `<xref>` targets, in particular
`libpq-connect-protocol_cursor` (new here) and `sql-declare`.

Client-side checks (no server needed beyond a connection):

```c
/* F5 */
PQsendBindWithCursorOptions(conn, "s", 65536, vals, NULL, NULL, 0, "p", 0);
/* expect 0, "number of parameters must be between 0 and 65535",
   connection still usable */
PQsendBindWithCursorOptions(conn, "s", -1, NULL, NULL, NULL, 0, "p", 0);
/* expect 0, same message */

/* F4 */
PQsendBindWithCursorOptions(conn, "s", 0, NULL, NULL, NULL, 0, "p",
							PQ_BIND_CURSOR_SCROLL | PQ_BIND_CURSOR_NO_SCROLL);
/* expect 0, rejected without a round trip */
```

```sh
# F6
valgrind --leak-check=full ./probe2 "$CONNINFO protocol_cursor=1"
# expect: no "definitely lost" block reachable from conninfo parsing
```

---

## Definition of done

- [ ] `nParams` bounded in `PQsendBindWithCursorOptions` using the existing
      message and `PQ_QUERY_PARAM_MAX_LIMIT`.
- [ ] `SCROLL｜NO_SCROLL` rejected client-side; `PQ_BIND_EXT_VALID_FLAGS` alias
      removed.
- [ ] `conn->protocol_cursor` freed in `freePGconn`; valgrind clean.
- [ ] `protocol_cursor` documented in the connection-parameter list, reachable
      via `<xref>`, and the `max_protocol_version` question in §D1 resolved one
      way or the other.
- [ ] `PQsendBindWithCursorOptions` documents the mutual exclusion, the
      single-`SELECT` requirement, and the `nParams` limit.
- [ ] Bind's optional `Int32` documented with its owning extension and numeric
      flag values.
- [ ] Fetch-count sentinel documented per direction; backward/absolute fetch
      documented as requiring `SCROLL`.
- [ ] Extension table shows the correct release and no libpq-specific constant
      names.
- [ ] `doc` build produces no new warnings.
