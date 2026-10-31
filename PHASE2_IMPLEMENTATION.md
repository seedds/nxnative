# Phase 2: Network Discovery Implementation Plan

Based on Eden emulator's LDN implementation analysis.

## Key Findings from Eden

### 1. Packet Types (from lan_discovery.cpp)
```cpp
enum LDNPacketType {
    Scan,           // Broadcast: looking for networks
    ScanResp,       // Response: here's my network info
    Connect,        // Client wants to join
    Disconnect,     // Client leaving
    DestroyNetwork, // Host destroying network
    SyncNetwork,    // Periodic network state sync
}
```

### 2. Structure Sizes (MUST MATCH EXACTLY)
- `NetworkInfo`: 0x480 bytes (1152 bytes)
- `NodeInfo`: 0x40 bytes (64 bytes)
- `Ssid`: 0x22 bytes (34 bytes) 
- `LdnNetworkInfo`: 0x430 bytes (1072 bytes)
- `MacAddress`: 6 bytes
- `Ipv4Address`: 4 bytes (array of u8[4])

### 3. Discovery Flow
1. **Host creates network:**
   - Generates random SessionId (128-bit)
   - Sets node 0 (self) as connected
   - Starts responding to scan broadcasts

2. **Client scans:**
   - Sends UDP broadcast (type: Scan)
   - Waits 1 second for responses
   - Filters responses by ScanFilter

3. **Host responds:**
   - Receives Scan packet
   - Sends ScanResp with full NetworkInfo

4. **Client connects:**
   - Sends Connect packet with NodeInfo
   - Host adds to connected_clients list
   - Host sends SyncNetwork updates

### 4. Network Layer (from network.h)
- Uses UDP sockets
- Broadcast to 255.255.255.255:11451
- Each packet has type + data payload
- IPv4 addresses stored in network byte order (reversed)

## Implementation Strategy

### Step 1: Add Complete Structure Definitions
```c
// In hle_offline.c, add full structures matching eden's ldn_types.h
typedef struct {
    uint8_t length;
    char raw[33];  // SsidLengthMax + 1
} LdnSsid;

typedef struct {
    uint8_t raw[4];  // Network byte order
} LdnIpv4Address;

typedef struct {
    uint8_t raw[6];
} LdnMacAddress;

// ... 20+ more structures
```

### Step 2: Add Socket Management
```c
static int g_ldn_socket = -1;
static const int LDN_PORT = 11451;

static int ldn_socket_init() {
    // Create UDP socket
    // Set SO_BROADCAST
    // Bind to LDN_PORT
    // Set non-blocking
}
```

### Step 3: Implement Scan with Broadcast
```c
Result h_ldn_Scan(...) {
    // Send UDP broadcast (type: Scan)
    // Wait 1 second
    // Collect ScanResp packets
    // Populate out array with NetworkInfo
}
```

### Step 4: Implement Create with Response
```c
Result h_ldn_Create(...) {
    // Build NetworkInfo structure
    // Generate random SessionId
    // Start background thread listening for Scan broadcasts
    // Respond with ScanResp when received
}
```

## File Changes Needed

**dm/host/hle_offline.c:**
- Add 20+ structure definitions (lines 20-200)
- Add socket init/cleanup (lines 201-250)
- Add packet send/receive (lines 251-350)
- Enhance h_ldn_Scan (lines 351-450)
- Enhance h_ldn_Create (lines 451-550)
- Enhance h_ldn_Ip to return real IP (lines 551-600)

**Estimated size:** +600 lines of C code

## Testing Plan

1. **Single device test:** Create network, scan finds own network
2. **Two device test:** Device A hosts, Device B scans and sees it
3. **Connection test:** Device B connects, both show connected state
4. **MK8D test:** Enter local wireless, see if menu works

## Risks & Issues

1. **iOS socket permissions** - May need entitlements
2. **Structure alignment** - MUST match Switch SDK exactly or crashes
3. **Background threads** - Need proper lifecycle management
4. **WiFi discovery** - May not work on all networks (firewalls, isolation)

## Next Action

Start with structure definitions - most critical for compatibility.
