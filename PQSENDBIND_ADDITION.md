# PQsendBindWithCursorOptions Function Addition

## Overview

Added `PQsendBindWithCursorOptions()` function to libpq to support creating portals with cursor options without immediately executing them. This enables proper testing of holdable portals that survive transaction commits.

## Function Signature

```c
int PQsendBindWithCursorOptions(PGconn *conn,
                                 const char *stmtName,
                                 int nParams,
                                 const char *const *paramValues,
                                 const int *paramLengths,
                                 const int *paramFormats,
                                 int resultFormat,
                                 const char *portalName,
                                 int cursorOptions);
```

## Difference from PQsendQueryPreparedWithCursorOptions

| Function | Protocol Messages Sent |
|----------|----------------------|
| `PQsendQueryPreparedWithCursorOptions` | Bind + Describe + Execute + Sync |
| `PQsendBindWithCursorOptions` | Bind + Describe + Sync |

The key difference is that `PQsendBindWithCursorOptions` does **not** send the Execute message, allowing the portal to be created but not executed immediately.

## Use Case

This function is essential for testing holdable cursors because:

1. Create a holdable portal with `PQsendBindWithCursorOptions()` (with cursor options 0x0020)
2. Commit the transaction
3. Execute/fetch from the portal after commit
4. This proves the portal survived the commit (holdable behavior)

With `PQsendQueryPreparedWithCursorOptions`, the Execute happens immediately, so you can't test whether the portal survives commit.

## Files Modified

- `src/interfaces/libpq/libpq-fe.h` - Added function declaration
- `src/interfaces/libpq/fe-exec.c` - Added function implementation
- `src/interfaces/libpq/exports.txt` - Added export at version 212
- `src/test/modules/libpq_pipeline/libpq_pipeline.c` - Updated test to use new function

## Test Implementation

The `test_holdable_cursor()` function now:

1. Prepares a statement
2. Calls `PQsendBindWithCursorOptions()` with cursor options 0x0020 (HOLD) to create portal "holdportal"
3. Commits the transaction
4. Fetches from the portal using SQL FETCH
5. Verifies 3 rows are returned (proving portal survived commit)
6. Closes the portal

## Result Sequence

When using `PQsendBindWithCursorOptions()`, the result sequence is:

1. PGRES_TUPLES_OK (RowDescription from Describe message)
2. NULL
3. (subsequent query results...)

Note: BindComplete message is consumed internally by libpq and not exposed as a separate result.

## Build Status

✅ Function compiles successfully
✅ Exports correctly
✅ Test passes with protocol 3.3
✅ Properly tests protocol-level cursor options in Bind message
