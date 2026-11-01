# Phase 4A: Polish LDN Implementation

## Goals
1. Connection timeout handling
2. Node state change events
3. Error handling improvements
4. Better logging/diagnostics

## Items

### 1. Connection Timeout (Host Side)
- Track last_seen_ns for each client
- Background thread checks for stale clients (no packet in 5 seconds)
- Auto-disconnect stale clients
- Broadcast updated network state

### 2. Node State Change Events
- Games call `AttachStateChangeEvent()` to get notified
- Signal event when:
  - New node connects
  - Node disconnects
  - Network state changes
- Store event handle and signal it

### 3. Error Handling
- Socket errors (init failure, bind failure)
- Packet parsing errors (invalid type, wrong size)
- State transition errors (wrong state for operation)
- Return proper Result codes

### 4. Logging
- Add debug logs for:
  - Packet sent/received (type, size, sender)
  - State transitions
  - Client connect/disconnect
  - Errors

## Implementation

### Step 1: Client Timeout (30 min)
- Add timeout check in listener thread
- Check every 1 second
- Remove clients with last_seen_ns > 5 seconds ago
- Broadcast SYNC after removal

### Step 2: State Change Events (30 min)
- Store event handle from AttachStateChangeEvent
- Signal on state/node changes
- Use existing os::SystemEvent infrastructure

### Step 3: Error Handling (20 min)
- Add Result codes for different error types
- Log errors before returning
- Graceful degradation (continue if possible)

### Step 4: Logging (10 min)
- Add LOG_DEBUG macros throughout
- Guard with compile flag or runtime debug level

## Estimated Time: 1.5 hours
