# Summary: Adding HOLD Support to Bind Message

## Quick Answer

To modify the Bind message to support the HOLD option, you need to:

1. **Add a cursor options field** to the end of the Bind message
2. **Parse the field** in `exec_bind_message()` 
3. **Apply options** to the portal after creation
4. **Validate constraints** (named portal, no FOR UPDATE, etc.)
5. **Update libpq** to send the new field

The existing `PreCommit_Portals()` infrastructure already handles holdable portals, so no changes needed there.

## Message Format Change

### Current Bind Message
```
'B' | length | portal | stmt | param_formats | params | result_formats
```

### Modified Bind Message
```
'B' | length | portal | stmt | param_formats | params | result_formats | cursor_options
                                                                         ^^^^^^^^^^^^^^
                                                                         NEW: Int32
```

## Minimal Code Changes

### 1. Backend: Parse Cursor Options

**File:** `src/backend/tcop/postgres.c:exec_bind_message()`

```c
// After reading result formats:
int cursorOptions = 0;

// Check if more data available (backward compatible)
if (input_message->cursor < input_message->len)
{
    cursorOptions = pq_getmsgint(input_message, 4);
}

// After PortalDefineQuery():
if (cursorOptions & CURSOR_OPT_HOLD)
{
    // Validate: named portal required
    if (portal_name[0] == '\0')
        ereport(ERROR, "holdable cursors require a named portal");
    
    // Validate: no security restrictions
    if (InSecurityRestrictedOperation())
        ereport(ERROR, "cannot create cursor WITH HOLD in restricted operation");
    
    // Validate: no row marks
    if (cplan->has_row_marks)
        ereport(ERROR, "WITH HOLD ... FOR UPDATE not supported");
}

portal->cursorOptions = cursorOptions;
```

### 2. Frontend: Send Cursor Options

**File:** `src/interfaces/libpq/fe-exec.c`

```c
int
PQsendQueryPreparedWithCursorOptions(PGconn *conn,
                                     const char *stmtName,
                                     int nParams,
                                     const char *const *paramValues,
                                     const int *paramLengths,
                                     const int *paramFormats,
                                     int resultFormat,
                                     const char *portalName,
                                     int cursorOptions)
{
    // ... build Bind message as usual ...
    
    // NEW: Append cursor options
    if (pqPutInt(cursorOptions, 4, conn) < 0)
        goto sendFailed;
    
    if (pqPutMsgEnd(conn) < 0)
        goto sendFailed;
    
    // ... send Execute and Sync ...
}
```

## Cursor Options Bitmask

```c
// From src/include/nodes/parsenodes.h
#define CURSOR_OPT_BINARY       0x0001  /* BINARY */
#define CURSOR_OPT_SCROLL       0x0002  /* SCROLL */
#define CURSOR_OPT_NO_SCROLL    0x0004  /* NO SCROLL */
#define CURSOR_OPT_INSENSITIVE  0x0008  /* INSENSITIVE */
#define CURSOR_OPT_ASENSITIVE   0x0010  /* ASENSITIVE */
#define CURSOR_OPT_HOLD         0x0020  /* WITH HOLD */
#define CURSOR_OPT_FAST_PLAN    0x0100  /* Prefer fast-start plan */
#define CURSOR_OPT_GENERIC_PLAN 0x0200  /* Force generic plan */
#define CURSOR_OPT_CUSTOM_PLAN  0x0400  /* Force custom plan */
#define CURSOR_OPT_PARALLEL_OK  0x0800  /* Allow parallel workers */
```

## Usage Example

```c
// Client code using libpq
PGconn *conn = PQconnectdb("...");

// Prepare statement
PQprepare(conn, "stmt1", "SELECT * FROM large_table WHERE id > $1", 1, NULL);

// Bind with HOLD option
PQsendQueryPreparedWithCursorOptions(
    conn,
    "stmt1",              // statement name
    1,                    // nParams
    (const char *[]){"100"},  // paramValues
    NULL,                 // paramLengths
    NULL,                 // paramFormats (text)
    0,                    // resultFormat (text)
    "my_cursor",          // portalName (REQUIRED for HOLD)
    0x0020                // cursorOptions (CURSOR_OPT_HOLD)
);

// Execute in transaction
PQexec(conn, "BEGIN");
PGresult *res = PQgetResult(conn);  // Get results
PQexec(conn, "COMMIT");

// Cursor survives commit!
PQexec(conn, "FETCH 10 FROM my_cursor");
```

## Validation Rules

### 1. Named Portal Required
```c
if ((cursorOptions & CURSOR_OPT_HOLD) && portal_name[0] == '\0')
    ERROR: "holdable cursors require a named portal"
```

**Reason:** Unnamed portals are destroyed on next Bind.

### 2. No Security Restrictions
```c
if ((cursorOptions & CURSOR_OPT_HOLD) && InSecurityRestrictedOperation())
    ERROR: "cannot create cursor WITH HOLD in restricted operation"
```

**Reason:** Security policy.

### 3. No Row Locking
```c
if ((cursorOptions & CURSOR_OPT_HOLD) && has_row_marks)
    ERROR: "WITH HOLD ... FOR UPDATE/SHARE not supported"
```

**Reason:** Locks can't be held across transactions.

### 4. Must Be in Transaction
```c
if ((cursorOptions & CURSOR_OPT_HOLD) && !IsTransactionBlock())
    ERROR: "CURSOR WITH HOLD requires transaction block"
```

**Reason:** HOLD only makes sense at commit boundary.

## How It Works

### 1. Bind Phase
```
Client → Server: Bind("my_cursor", "stmt1", params, 0x0020)
Server: Creates portal with cursorOptions = 0x0020
Server → Client: BindComplete
```

### 2. Execute Phase
```
Client → Server: Execute("my_cursor", 0)
Server: Runs query, returns rows
Server → Client: DataRow, DataRow, ..., CommandComplete
```

### 3. Commit Phase
```
Client → Server: COMMIT
Server: Calls PreCommit_Portals()
        Detects portal->cursorOptions & CURSOR_OPT_HOLD
        Calls HoldPortal(portal)
            → PortalCreateHoldStore()  // Create tuplestore
            → PersistHoldablePortal()  // Materialize rows
        Portal survives commit!
Server → Client: CommandComplete
```

### 4. Post-Commit Access
```
Client → Server: Execute("my_cursor", 10)
Server: Reads from materialized tuplestore
Server → Client: DataRow × 10
```

## Backward Compatibility

### Protocol 3.0-3.2 Clients
- Don't send cursor options field
- `input_message->cursor < input_message->len` is false
- `cursorOptions` defaults to 0
- **No breaking changes**

### Protocol 3.3 Clients
- Send cursor options field
- Server reads it if present
- **Opt-in feature**

## Files to Modify

1. **src/backend/tcop/postgres.c** - Parse cursor options in Bind
2. **src/backend/libpq/pqformat.c** - Add `pq_getmsgend_if_done()`
3. **src/interfaces/libpq/fe-exec.c** - Add `PQsendQueryPreparedWithCursorOptions()`
4. **src/interfaces/libpq/libpq-fe.h** - Export new function
5. **src/include/libpq/pqformat.h** - Declare `pq_getmsgend_if_done()`
6. **doc/src/sgml/protocol.sgml** - Document new field

## Testing

```sql
-- Test 1: Basic HOLD functionality
BEGIN;
-- Bind with HOLD
-- Execute
COMMIT;
-- Execute again (should work)

-- Test 2: Unnamed portal with HOLD (should fail)
-- Bind("", "stmt", ..., 0x0020)  → ERROR

-- Test 3: HOLD with FOR UPDATE (should fail)
-- Prepare "SELECT ... FOR UPDATE"
-- Bind with HOLD → ERROR

-- Test 4: Backward compatibility
-- Old client (no cursor options field) → works normally
```

## Performance Impact

### Memory
- Holdable cursors materialize entire result set
- O(n) memory where n = number of rows
- Use tuplestore (can spill to disk)

### Planning
- No impact on query planning
- Cursor options applied after planning

### Execution
- First Execute: Normal execution
- Commit: Materializes all remaining rows
- Subsequent Execute: Reads from tuplestore (fast)

## Advantages Over SQL DECLARE

### Current (SQL DECLARE)
```sql
BEGIN;
DECLARE my_cursor CURSOR WITH HOLD FOR SELECT * FROM table WHERE id > $1;
-- Can't use parameters easily
FETCH 10 FROM my_cursor;
COMMIT;
```

### Proposed (Protocol Bind)
```c
// Full parameterization support
PQprepare(conn, "stmt", "SELECT * FROM table WHERE id > $1", 1, NULL);
PQsendQueryPreparedWithCursorOptions(conn, "stmt", 1, params, ..., "cursor", 0x0020);
// Cleaner, more efficient
```

## See Also

- **bind_message_hold_modification.md** - Detailed implementation guide
- **bind_hold_poc.patch** - Proof-of-concept patch
- **src/backend/utils/mmgr/portalmem.c** - Portal lifecycle management
- **src/backend/commands/portalcmds.c** - SQL DECLARE CURSOR implementation
