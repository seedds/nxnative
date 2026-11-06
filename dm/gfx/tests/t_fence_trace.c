#include "../nvn/nvn_fence_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1024
static ft_event events[N * 4];
static ft_buffer trace;
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void *writer(void *arg) {
    uint64_t id = (uintptr_t)arg;
    for (uint64_t i = 0; i < N; i++) {
        ft_event e = {.kind=FT_FEEDBACK, .thread=id, .fence=id*N+i,
                      .cpu_ns=i, .gpu_begin_s=(double)i, .gpu_end_s=(double)i+1};
        ft_append(&trace, &e);
    }
    return NULL;
}
int main(void) {
    ft_init(&trace, events, N*4);
    pthread_t threads[4];
    for (uintptr_t i=0;i<4;i++) if (pthread_create(&threads[i],NULL,writer,(void *)i)) return 2;
    for (int i=0;i<4;i++) if (pthread_join(threads[i],NULL)) return 2;
    ft_cutoff c=ft_close(&trace); CHECK(c.used==N*4); CHECK(c.dropped==0);
    unsigned char seen[N*4]={0};
    for (size_t i=0;i<c.used;i++) {
        ft_event e=events[i]; CHECK(e.fence<N*4);
        if (e.fence<N*4) { CHECK(!seen[e.fence]); seen[e.fence]=1; }
        CHECK(e.fence==e.thread*N+e.cpu_ns); CHECK(e.gpu_end_s==e.gpu_begin_s+1);
    }
    ft_event late={.kind=FT_FEEDBACK,.fence=999999};
    CHECK(!ft_append(&trace,&late)); CHECK(trace.used==N*4);
    CHECK(events[N*4-1].fence!=999999);
    pthread_mutex_destroy(&trace.lock);
    memset(events,0,sizeof events); ft_init(&trace,events,2);
    ft_event end={.kind=FT_SYNC_END,.sync=7,.fence=44};
    ft_event begin={.kind=FT_SYNC_BEGIN,.sync=7,.fence=44};
    CHECK(ft_append(&trace,&end)); CHECK(ft_append(&trace,&begin));
    CHECK(!ft_append(&trace,&late)); c=ft_close(&trace);
    CHECK(c.used==2); CHECK(c.dropped==1);
    CHECK(events[0].kind==FT_SYNC_END && events[1].kind==FT_SYNC_BEGIN);
    CHECK(events[0].sync==events[1].sync && events[0].fence==44);
    pthread_mutex_destroy(&trace.lock);
    printf("fence trace: %d failures; 4096 concurrent events retained, overflow and cutoff verified\n",failures);
    return failures!=0;
}
