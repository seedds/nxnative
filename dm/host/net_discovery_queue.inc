// Discovery-only UDP fanout. A duplicate retains the original kernel socket,
// source port and socket options. Slot generations cancel work on close/reuse.
// Queuing a datagram has UDP's usual best-effort delivery semantics; it is not
// an acknowledgement from a console. No ordinary unicast enters this queue.
enum { NET_DISCOVERY_JOBS = 16, NET_DISCOVERY_PER_SOCKET = 2, NET_DISCOVERY_BYTES = 32768 };
typedef struct {
    net_ref socket; dm_net_discovery_plan plan; struct sockaddr_in destination;
    size_t size; int flags; uint64_t expires; unsigned char bytes[];
} net_discovery_job;
static pthread_mutex_t g_discovery_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_discovery_changed = PTHREAD_COND_INITIALIZER;
static net_discovery_job *g_discovery_jobs[NET_DISCOVERY_JOBS];
static int g_discovery_started;
#ifdef DM_NET_TEST
static _Atomic(dm_net_test_discovery_sender) g_discovery_sender;
void dm_net_test_set_discovery_sender(dm_net_test_discovery_sender sender) { atomic_store(&g_discovery_sender,sender); }
#endif
static long net_discovery_packet(int fd,const void *bytes,size_t size,int flags,const struct sockaddr_in *destination) {
#ifdef DM_NET_TEST
    dm_net_test_discovery_sender sender=atomic_load(&g_discovery_sender);
    if(sender)return sender(fd,bytes,size,flags,destination->sin_addr.s_addr,destination->sin_port);
#endif
    return sendto(fd,bytes,size,flags,(const struct sockaddr *)destination,sizeof *destination);
}
long dm_net_discovery_send_native(int fd,const void *bytes,size_t size,int flags,uint32_t address,uint16_t port) {
    struct sockaddr_in destination={.sin_len=sizeof destination,.sin_family=AF_INET,.sin_port=port,.sin_addr={address}};
    return net_discovery_packet(fd,bytes,size,flags,&destination);
}
static void net_discovery_drop(unsigned i) {
    net_discovery_job *job = g_discovery_jobs[i];
    if (job) { close(job->socket.fd); free(job); g_discovery_jobs[i] = NULL; }
}
static void net_discovery_cancel(int guest, uint64_t generation) {
    pthread_mutex_lock(&g_discovery_lock);
    for (unsigned i = 0; i < NET_DISCOVERY_JOBS; ++i)
        if (g_discovery_jobs[i] && g_discovery_jobs[i]->socket.guest == guest &&
            g_discovery_jobs[i]->socket.generation == generation) net_discovery_drop(i);
    pthread_cond_signal(&g_discovery_changed); pthread_mutex_unlock(&g_discovery_lock);
}
static void *net_discovery_worker(void *unused) {
    (void)unused; unsigned cursor = 0; uint64_t next = 0;
    pthread_mutex_lock(&g_discovery_lock);
    for (;;) {
        unsigned i, n;
        for (n = 0; n < NET_DISCOVERY_JOBS; ++n) {
            i = (cursor + n) % NET_DISCOVERY_JOBS;
            net_discovery_job *job = g_discovery_jobs[i];
            if (job && (!dm_net_enabled() || !net_live(&job->socket) || net_ms() >= job->expires)) net_discovery_drop(i);
            if (g_discovery_jobs[i]) break;
        }
        if (n == NET_DISCOVERY_JOBS) { pthread_cond_wait(&g_discovery_changed, &g_discovery_lock); continue; }
        uint64_t now = net_ms();
        if (now < next) {
            struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += (long)(next - now) * 1000000L;
            deadline.tv_sec += deadline.tv_nsec / 1000000000L; deadline.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&g_discovery_changed, &g_discovery_lock, &deadline); continue;
        }
        net_discovery_job *job = g_discovery_jobs[i]; unsigned sent = 0, bytes = 0;
        for (; sent < DM_DISCOVERY_BATCH; ++sent) {
            if (sent && bytes + job->size > NET_DISCOVERY_BYTES) break;
            dm_net_discovery_plan saved = job->plan; uint32_t address;
            if (!dm_net_discovery_next(&job->plan, &address)) { net_discovery_drop(i); break; }
            job->destination.sin_addr.s_addr = address;
            ssize_t result = net_discovery_packet(job->socket.fd, job->bytes, job->size, job->flags,&job->destination);
            if (result < 0 && (errno == EAGAIN || errno == ENOBUFS || errno == EINTR)) { job->plan = saved; break; }
            if(result<0&&!dm_net_discovery_unreachable(errno)) { net_discovery_drop(i);break; }
            // An unreachable neighbor does not prevent discovery of others.
            bytes += (unsigned)job->size;
        }
        cursor = (i + 1) % NET_DISCOVERY_JOBS; next = net_ms() + DM_DISCOVERY_TICK_MS;
    }
    return NULL;
}
static long net_discovery_enqueue(const net_ref *socket, dm_net_discovery_plan plan,
                                  struct sockaddr_in destination, const void *bytes, size_t size, int flags) {
    if (size > 65507) return net_error(90);
    net_discovery_job *job = malloc(sizeof *job + size);
    if (!job) return net_error(12);
    *job = (net_discovery_job){.socket = *socket, .plan = plan, .destination = destination,
                               .size = size, .flags = flags, .expires = net_ms() + 30000};
    if (size) memcpy(job->bytes, bytes, size);
    pthread_mutex_lock(&g_discovery_lock); unsigned free_slot = NET_DISCOVERY_JOBS, same = 0;
    for (unsigned i = 0; i < NET_DISCOVERY_JOBS; ++i) {
        net_discovery_job *old = g_discovery_jobs[i];
        if (!old) { free_slot = i; continue; }
        if (old->socket.guest != socket->guest || old->socket.generation != socket->generation) continue;
        ++same;
        if (old->size == size && old->flags == flags && old->destination.sin_port == destination.sin_port &&
            old->plan.first == plan.first && old->plan.last == plan.last && old->plan.local == plan.local &&
            (!size || !memcmp(old->bytes, bytes, size))) {
            // Repeated identical queries during one sweep do not multiply it.
            pthread_mutex_unlock(&g_discovery_lock); free(job); return (long)size;
        }
    }
    if (free_slot == NET_DISCOVERY_JOBS || same >= NET_DISCOVERY_PER_SOCKET) {
        pthread_mutex_unlock(&g_discovery_lock); free(job); return net_error(105);
    }
    if (!g_discovery_started) {
        pthread_t thread; int error = pthread_create(&thread, NULL, net_discovery_worker, NULL);
        if (error) { pthread_mutex_unlock(&g_discovery_lock); free(job); return net_error(net_errno(error)); }
        pthread_detach(thread); g_discovery_started = 1;
    }
    job->socket.fd = fcntl(socket->fd, F_DUPFD_CLOEXEC, 0);
    if (job->socket.fd < 0) { int error = errno; pthread_mutex_unlock(&g_discovery_lock); free(job); return net_error(net_errno(error)); }
    ssize_t result=(ssize_t)size;
    for(unsigned attempt=0;attempt<DM_DISCOVERY_BATCH;++attempt) {
        uint32_t first;
        if(!dm_net_discovery_next(&job->plan,&first)) {
            int error=errno;close(job->socket.fd);pthread_mutex_unlock(&g_discovery_lock);free(job);return net_error(net_errno(error));
        }
        job->destination.sin_addr.s_addr=first;
        result=net_discovery_packet(job->socket.fd,bytes,size,flags,&job->destination);
        if(result>=0)break;
        int error=errno;
        if(dm_net_discovery_unreachable(error)) { result=(ssize_t)size;continue; }
        close(job->socket.fd);pthread_mutex_unlock(&g_discovery_lock);free(job);return net_error(net_errno(error));
    }
    g_discovery_jobs[free_slot] = job; pthread_cond_signal(&g_discovery_changed);
    pthread_mutex_unlock(&g_discovery_lock); return result;
}
#ifdef DM_NET_TEST
unsigned dm_net_test_discovery_pending(void) {
    unsigned count = 0; pthread_mutex_lock(&g_discovery_lock);
    for (unsigned i = 0; i < NET_DISCOVERY_JOBS; ++i) count += g_discovery_jobs[i] != NULL;
    pthread_mutex_unlock(&g_discovery_lock); return count;
}
#endif
