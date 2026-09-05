# Protocol Documentation Updates for Version 3.3

## File Updated
`doc/src/sgml/protocol.sgml`

## Changes Made

### 1. Protocol Version Updated
- Changed current protocol version from 3.2 to 3.3 throughout document
- Updated introduction section to reference version 3.3

### 2. Protocol Versions Table
Added new entry for protocol 3.3:
- **Version:** 3.3
- **Supported by:** PostgreSQL 18 and later
- **Description:** Current latest version. The Bind message now supports an optional cursor options field to control portal behavior, including the ability to create holdable portals that survive transaction commit.

### 3. Bind Message Format
Added new optional field to Bind message specification:
- **Field:** Int32 (cursor options)
- **When:** Protocol 3.3 and later
- **Description:** Bitmask of options for the portal being created
- **Defined bits:**
  - `0x0001` (CURSOR_OPT_BINARY) - Same as setting result format codes to binary
  - `0x0020` (CURSOR_OPT_HOLD) - Creates a holdable portal that survives transaction commit
- **Notes:** 
  - Field is optional; if not present, no cursor options are set
  - Named portals are required when using CURSOR_OPT_HOLD

### 4. Bind Message Description
Updated the Bind message description section to mention:
- Protocol 3.3 adds optional cursor options
- Cursor options control portal behavior
- Holdable portals survive transaction commit

### 5. Portal Lifetime Description
Updated portal lifetime documentation to explain:
- Standard portals last until end of transaction
- Portals created with CURSOR_OPT_HOLD are holdable
- Holdable portals survive transaction commit
- Holdable portals remain valid until explicitly closed or session ends

## Backward Compatibility
- The cursor options field is optional and backward compatible
- Servers detect the field by checking if additional bytes remain in the Bind message
- Clients using protocol 3.0-3.2 continue to work without changes
- Protocol 3.3 clients can optionally use the new cursor options field

## Related Code Changes
- `src/include/libpq/pqcomm.h` - PG_PROTOCOL_LATEST set to 3.3
- `src/backend/tcop/postgres.c` - Bind message parsing updated
- `src/interfaces/libpq/fe-exec.c` - PQsendQueryPreparedWithCursorOptions() added
- `src/test/modules/libpq_pipeline/libpq_pipeline.c` - test_holdable_cursor() added
