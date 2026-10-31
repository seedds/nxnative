# Phase 3: Background Listener & Connection Handling

## Goal
Enable **actual multiplayer** - devices can see each other, connect, and exchange data.

## What's Missing (Phase 1+2 → Phase 3)

### Current Behavior
- Host creates network ✅
- Client scans and sends broadcast ✅
- **Host doesn't auto-respond** ❌ (needs background thread)
- **No connection handling** ❌
- **No data transfer** ❌

### Phase 3 Additions

#### 1. Background Listener Thread (pthread)
```c
static pthread_t g_listener_thread;
static _Atomic int g_listener_running = 0;

void* ldn_listener_thread(void* arg) {
    while (atomic_load(&g_listener_running)) {
        uint8_t type;
        LdnFullNetworkInfo info;
        size_t len;
        char sender_ip[INET_ADDRSTRLEN];
        
        if (ldn_receive_packet_with_sender(&type, &info, &len, sizeof(info), sender_ip)) {
            switch(type) {
                case LDN_PACKET_SCAN:
                    if (atomic_load(&g_ldn_state) == LDN_STATE_ACCESS_POINT_CREATED) {
                        ldn_send_packet(LDN_PACKET_SCAN_RESP, &g_network_info, 
                                       sizeof(g_network_info), sender_ip);
                    }
                    break;
                case LDN_PACKET_CONNECT:
                    // Handle connect request
                    break;
                case LDN_PACKET_DISCONNECT:
                    // Handle disconnect
                    break;
            }
        }
        usleep(10000); // 10ms poll
    }
    return NULL;
}
```

#### 2. Connection Handling
- **Connect packet**: Client sends NodeInfo to host
- **Host**: Adds client to connected_clients array, assigns node ID
- **SyncNetwork packet**: Host broadcasts updated NetworkInfo to all clients
- **Client**: Receives SyncNetwork, transitions to STATION_CONNECTED

#### 3. Data Transfer (Game Data)
- New packet type: `LDN_PACKET_DATA`
- Game calls `nn::ldn::SendData()` → UDP to all connected nodes
- Game calls `nn::ldn::ReceiveData()` → read from receive buffer
- Buffer per-node data in thread-safe queue

#### 4. Node Management
- Track connected clients (IP + MAC + node ID)
- Handle disconnections (timeouts, explicit disconnect packets)
- Update node_count in NetworkInfo
- Notify game via state change events

## Implementation Order

### Step 1: Background Thread (1-2 hours)
- Add pthread to CreateNetwork
- Auto-respond to scan packets
- Test: Client should see host in scan results

### Step 2: Connect/Disconnect (1-2 hours)
- Handle CONNECT packet from client
- Send SYNC packet to update all nodes
- Handle DISCONNECT gracefully
- Test: Client connects, appears in host's node list

### Step 3: Data Transfer (1-2 hours)
- Implement SendData/ReceiveData functions
- Thread-safe data queues per node
- Test: Exchange simple messages

### Step 4: Polish (30 min)
- Connection timeouts
- Error handling
- Cleanup on Finalize

## Eden Reference Points

**lan_discovery.cpp lines 480-549**: ReceivePacket handler
```cpp
case Network::LDNPacketType::Scan: {
    if (state == State::AccessPointCreated) {
        SendPacket(Network::LDNPacketType::ScanResp, network_info, packet.local_ip);
    }
    break;
}
case Network::LDNPacketType::Connect: {
    NodeInfo info{};
    std::memcpy(&info, packet.data.data(), sizeof(NodeInfo));
    connected_clients.push_back(packet.local_ip);
    // Assign to first free station
    UpdateNodes(); // Sends SyncNetwork to all clients
    break;
}
```

## Testing Plan

1. **Build for iOS** (requires Mac)
2. **Two devices** on same WiFi
3. **Device A**: Host creates network
4. **Device B**: Scan → should see Device A
5. **Device B**: Connect → should join
6. **Both**: Exchange game data

## Estimated Time: 4-5 hours
