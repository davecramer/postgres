# Holdable Cursor Test in libpq_pipeline.c

## Overview

Added `test_holdable_cursor()` function to the PostgreSQL libpq_pipeline test suite to verify that protocol 3.3 cursor options work correctly in pipeline mode.

## Test Location

**File:** `src/test/modules/libpq_pipeline/libpq_pipeline.c`

## Test Implementation

The test verifies protocol 3.3 cursor options functionality using `PQsendQueryPreparedWithCursorOptions()`:

1. Verifies protocol 3.3 is active
2. Creates a temporary table with test data (3 rows)
3. Prepares a SELECT statement
4. Enters pipeline mode
5. Creates a named portal with HOLD cursor option (0x0020) using Bind message
6. Commits the transaction (portal should survive)
7. Verifies all 3 rows are returned
8. Exits pipeline mode

## Running the Test

```bash
# Run with protocol 3.3 (required)
./build/src/test/modules/libpq_pipeline/libpq_pipeline holdable_cursor "host=localhost port=5432 dbname=postgres max_protocol_version=3.3"

# List all available tests
./build/src/test/modules/libpq_pipeline/libpq_pipeline tests
```

## Expected Output

```
holdable cursor... ok
```

## Test Verification

The test verifies:
- Protocol 3.3 cursor options field in Bind message
- `PQsendQueryPreparedWithCursorOptions()` works in pipeline mode
- Named portal "holdportal" created with HOLD option (0x0020)
- Portal survives transaction commit
- Proper result count (3 rows)

## Implementation Notes

- Uses `PQsendQueryPreparedWithCursorOptions()` to send Bind message with cursor options
- Tests protocol-level cursor options (not SQL DECLARE CURSOR)
- Requires protocol 3.3 connection
- Follows existing libpq_pipeline test patterns
- Minimal implementation focused on protocol 3.3 functionality

## Build Status

✅ Test compiles successfully
✅ Test executes successfully with protocol 3.3
✅ Test appears in test list
✅ Tests protocol 3.3 cursor options in Bind message
