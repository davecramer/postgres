# Migration from Protocol 3.3 to Protocol Options

## Summary

Changed the holdable cursor implementation from using protocol version 3.3 with flags in the Bind message to using protocol options with `_pq_.holdable_portal`.

## Changes Made

### 1. Protocol Version (src/include/libpq/pqcomm.h)
- Reverted `PG_PROTOCOL_LATEST` from `PG_PROTOCOL(3,3)` back to `PG_PROTOCOL(3,2)`

### 2. Backend Changes

#### src/include/libpq/libpq-be.h
- Added `bool holdable_portal_enabled` field to `Port` structure

#### src/backend/tcop/backend_startup.c
- Modified `_pq_.` option handling to recognize `_pq_.holdable_portal`
- Parse boolean value and set `port->holdable_portal_enabled`
- Unrecognized `_pq_.` options still added to `unrecognized_protocol_options` list

#### src/backend/tcop/postgres.c (exec_bind_message)
- Changed from checking protocol version to checking `MyProcPort->holdable_portal_enabled`
- Only read cursor options from Bind message if `_pq_.holdable_portal` was enabled

### 3. Client (libpq) Changes

#### src/interfaces/libpq/libpq-int.h
- Added `char *holdable_portal` field to `pg_conn` structure for connection parameter
- Added `bool holdable_portal_enabled` field to track parsed state

#### src/interfaces/libpq/fe-connect.c
- Added `holdable_portal` connection parameter (default "0")
- Can be set via connection string: `holdable_portal=1`

#### src/interfaces/libpq/fe-protocol3.c (build_startup_packet)
- Check if `conn->holdable_portal` is "1"
- If enabled, add `_pq_.holdable_portal=true` to startup packet
- Set `conn->holdable_portal_enabled = true`

#### src/interfaces/libpq/fe-exec.c
- Changed `PQsendQueryPreparedWithCursorOptions()` to check `conn->holdable_portal_enabled` instead of protocol version
- Changed `PQsendBindWithCursorOptions()` to check `conn->holdable_portal_enabled` instead of protocol version
- Only send cursor options field in Bind message if enabled

## Usage

### Client Connection String
```c
conn = PQconnectdb("dbname=postgres holdable_portal=1");
```

### Startup Packet
When `holdable_portal=1` is set, the client sends:
```
_pq_.holdable_portal=true
```

### Bind Message
When `_pq_.holdable_portal` is enabled, the Bind message includes the optional cursor options field:
```
Int32(cursorOptions)  // e.g., 0x0020 for CURSOR_OPT_HOLD
```

## Benefits

1. **Backward Compatible**: Uses protocol 3.2, no new protocol version needed
2. **Opt-in**: Feature must be explicitly enabled via connection parameter
3. **Standard Mechanism**: Uses existing `_pq_.` protocol option infrastructure
4. **Negotiable**: Server can report if option is not supported via NegotiateProtocolVersion message

## Testing

Update existing tests to use `holdable_portal=1` connection parameter instead of `min_protocol_version=3.3`:

```c
// Old
conn = PQconnectdb("dbname=postgres min_protocol_version=3.3");

// New
conn = PQconnectdb("dbname=postgres holdable_portal=1");
```
