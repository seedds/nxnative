// Exercises the actual host LAN implementation against a real UDP receiver.
// make -C dm/host/tests GAME=<title ID> lan
#include "../hle_net.c"
#include <assert.h>
void dm_guest_sleep_ns(int64_t ns) { struct timespec t = {ns / 1000000000, ns % 1000000000}; nanosleep(&t, NULL); }
void dm_backtrace(uint64_t fp) { (void)fp; } // diagnostic-only host callback

int main(void) {
    alarm(15);
    // Explicit loopback selection keeps this test independent of a LAN adapter.
    setenv("DM_LAN_IP", "127.0.0.1", 1);
    setenv("DM_LAN", "1", 1);
    assert(h_socket_InternalInit(3) == RES_OK && h_socket_IsInitialized());
    assert(sk_errno(EINPROGRESS) == 115 && sk_errno(ECONNRESET) == 104 && sk_errno(EAGAIN) == 11);
    uint32_t ip, mask;
    unsetenv("DM_LAN_PEERS"); setenv("DM_LAN", "0", 1);
    assert(!dm_nifm_ipv4(&ip, &mask) && !ip && !mask);
    assert(dm_sk_socket(AF_INET, SOCK_DGRAM, 0) == -1);
    assert(dm_sk_poll(NULL, 0, 1) == 0);
    // SDK 21's offline HTTP loop can poll an unavailable descriptor; upstream
    // treats it as nothing ready rather than aborting the game with ENETDOWN.
    struct pollfd unavailable = {.fd = INT_MAX, .events = POLLIN, .revents = POLLNVAL};
    assert(dm_sk_poll(&unavailable, 1, 1) == 0 && unavailable.revents == 0);
    int receiver = socket(AF_INET, SOCK_DGRAM, 0), sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
    struct sockaddr_in local = {.sin_len = sizeof(local), .sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(dm_sk_bind(sender, &local, sizeof(local)) == -1 && t_skerr == 100);
    assert(bind(receiver, (struct sockaddr *)&local, sizeof(local)) == 0);
    socklen_t alen = sizeof(local);
    assert(getsockname(receiver, (struct sockaddr *)&local, &alen) == 0);
    struct timeval timeout = {.tv_sec = 1};
    assert(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    setenv("DM_LAN", "1", 1); setenv("DM_LAN_PEERS", "127.0.0.1", 1);
    assert(dm_sk_fcntl(sender, F_SETFL, 0x800) == 0);
    assert(fcntl(sender, F_GETFL) & O_NONBLOCK);
    assert(dm_sk_fcntl(sender, F_GETFL, 0) == 0x800);
    assert(dm_sk_fcntl(sender, F_SETFL, 0) == 0);
    assert(!(fcntl(sender, F_GETFL) & O_NONBLOCK));
    assert(dm_nifm_ipv4(&ip, &mask) && ip && mask == htonl(0xffffff00));
    struct sockaddr_in broadcast = local;
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char payload[] = "Mario Kart LAN discovery probe";
    // No SO_BROADCAST: local broadcast fails, but the explicit peer copy must arrive.
    assert(dm_sk_sendto(sender, payload, sizeof(payload), 0, &broadcast, sizeof(broadcast)) == sizeof(payload));
    struct pollfd p = {.fd = receiver, .events = POLLIN};
    assert(dm_sk_poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
    char buf[128];
    assert(h_sk_Recv(receiver, buf, sizeof(buf), MSG_PEEK) == sizeof(payload));
    assert(h_sk_Recv(receiver, buf, 4, 0) == 4 && !memcmp(buf, payload, 4));
    assert(h_sk_Recv(receiver, buf, sizeof(buf), 0) == -1 && h_sk_GetLastError() == 11);
    uint32_t real_peer = htonl(INADDR_LOOPBACK), virtual_peer = sk_address(real_peer, 0);
    assert(virtual_peer != real_peer && sk_address(virtual_peer, 1) == real_peer);
    assert(sk_address(ip, 1) != ip);
    local.sin_addr.s_addr = virtual_peer;
    assert(dm_sk_sendto(sender, payload, sizeof(payload), 0, &local, sizeof(local)) == sizeof(payload));
    assert(h_sk_Recv(receiver, buf, sizeof(buf), 0) == sizeof(payload));
    unsigned char small[32]; memset(small, 0x5a, sizeof(small));
    socklen_t short_len = 4;
    assert(dm_sk_name(receiver, small, &short_len, 0) == 0 && short_len >= sizeof(struct sockaddr_in));
    for (int i = 4; i < sizeof(small); i++) assert(small[i] == 0x5a);
    struct iovec iov = {.iov_base = (void *)payload, .iov_len = sizeof(payload)};
    struct msghdr msg = {.msg_name = &local, .msg_namelen = sizeof(local), .msg_iov = &iov, .msg_iovlen = 1};
    assert(dm_sk_sendmsg(sender, &msg, 0) == sizeof(payload));
    struct sockaddr_in source;
    iov.iov_base = buf; iov.iov_len = sizeof(buf); msg.msg_name = &source;
    assert(dm_sk_recvmsg(receiver, &msg, 0) == sizeof(payload));
    assert(source.sin_addr.s_addr == virtual_peer);
    assert(dm_sk_sendto(sender, payload, sizeof(payload), 0, &local, sizeof(local)) == sizeof(payload));
    memset(small, 0x5a, sizeof(small)); short_len = 4;
    assert(dm_sk_recvfrom(receiver, buf, sizeof(buf), 0, small, &short_len) == sizeof(payload));
    for (int i = 4; i < sizeof(small); i++) assert(small[i] == 0x5a);
    assert(dm_sk_sendto(sender, payload, sizeof(payload), 0, &local, sizeof(local)) == sizeof(payload));
    memset(small, 0x5a, sizeof(small));
    iov.iov_base = buf; iov.iov_len = sizeof(buf); msg.msg_name = small; msg.msg_namelen = 4;
    assert(dm_sk_recvmsg(receiver, &msg, 0) == sizeof(payload));
    for (int i = 4; i < sizeof(small); i++) assert(small[i] == 0x5a);
    local.sin_addr.s_addr = inet_addr("203.0.113.1");
    assert(dm_sk_sendto(sender, payload, sizeof(payload), 0, &local, sizeof(local)) == -1 && t_skerr == EACCES);
    close(receiver); close(sender);
    // A discovered peer must also be able to establish a native TCP connection.
    int listener = socket(AF_INET, SOCK_STREAM, 0), client = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0 && client >= 0);
    local.sin_addr.s_addr = ip; local.sin_port = 0;
    assert(dm_sk_bind(listener, &local, sizeof(local)) == 0);
    assert(dm_sk_listen(listener, 1) == 0);
    alen = sizeof(local);
    assert(getsockname(listener, (struct sockaddr *)&local, &alen) == 0);
    assert(dm_sk_connect(client, &local, sizeof(local)) == 0);
    struct sockaddr_in remote; alen = sizeof(remote);
    int accepted = dm_sk_accept(listener, &remote, &alen);
    assert(accepted >= 0);
    assert(h_sk_Send(client, payload, sizeof(payload), 0) == sizeof(payload));
    assert(h_sk_Recv(accepted, buf, sizeof(buf), 0) == sizeof(payload));
    assert(!memcmp(buf, payload, sizeof(payload)));
    close(accepted); close(client); close(listener);
    int ping = dm_sk_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    assert(ping >= 0);
    int kind = 0; socklen_t kindlen = sizeof(kind);
    assert(dm_sk_getsockopt(ping, SOL_SOCKET, SO_TYPE, &kind, &kindlen) == 0 && kind == SOCK_RAW);
    assert(dm_sk_fcntl(ping, F_SETFD, FD_CLOEXEC) == 0);
    assert(dm_sk_fcntl(ping, F_GETFD, 0) & FD_CLOEXEC);
    assert(setsockopt(ping, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    unsigned char echo[16] = {8,0,0,0,0x12,0x34,0,1,'l','a','n','-','t','e','s','t'};
    unsigned checksum = 0;
    for (int i = 0; i < 16; i += 2) checksum += (echo[i] << 8) | echo[i+1];
    while (checksum >> 16) checksum = (checksum & 65535) + (checksum >> 16);
    checksum = ~checksum; echo[2] = checksum >> 8; echo[3] = checksum;
    local.sin_addr.s_addr = virtual_peer; local.sin_port = 0;
    assert(dm_sk_sendto(ping, echo, sizeof(echo), 0, &local, sizeof(local)) == sizeof(echo));
    alen = sizeof(source);
    long reply = dm_sk_recvfrom(ping, buf, sizeof(buf), 0, &source, &alen);
    assert(reply >= 36 && source.sin_addr.s_addr == virtual_peer);
    uint32_t header_source; memcpy(&header_source, buf + 12, 4);
    assert(header_source == virtual_peer);
    unsigned header_length = ((unsigned char)buf[0] & 15) * 4;
    assert(buf[header_length] == 0 && !memcmp(buf + header_length + 4, echo + 4, 12));
    assert(dm_sk_close(ping) == 0 && !sk_is_icmp(ping));
    ping = dm_sk_socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    assert(ping >= 0);
    assert(dm_sk_getsockopt(ping, SOL_SOCKET, SO_TYPE, &kind, &kindlen) == 0 && kind == SOCK_DGRAM);
    assert(dm_sk_getsockopt(ping, SOL_SOCKET, SO_TYPE, NULL, &kindlen) == -1 && t_skerr == EFAULT);
    assert(dm_sk_bind(ping, NULL, 0) == -1 && t_skerr == EFAULT);
    assert(dm_sk_close(ping) == 0);
    puts("PASS: SDK initialization, LAN opt-in, peer broadcast, virtual address translation, sendmsg/recvmsg, TCP connect/accept, ICMP echo with virtual IPv4 header, poll, peek, truncation, receive timeout, destination restriction");
}
