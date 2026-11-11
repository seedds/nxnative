#include <pthread.h>
// Opt-in CPU breadcrumb: the exact bind packet recorded by this guest thread.
// Reading a PROG header is not a lifetime/ownership proof; print it as evidence only.
void crash_context_dump_register(void (*fn)(const void *));
static int g_bind_diagnostics;
static _Thread_local struct {uint64_t sequence;const void *command,*program;int stages,readable;prog_t header;} g_bind_last;
static void bind_diagnostics_dump(const void *unused) {
    (void)unused;if(!g_bind_diagnostics)return;
    fprintf(stderr,"[dm] BIND_DIAGNOSTIC sequence=%llu command=%p program=%p stages=%#x readable=%d magic=%#x nstage=%u serial=%llu\n",(unsigned long long)g_bind_last.sequence,g_bind_last.command,g_bind_last.program,g_bind_last.stages,g_bind_last.readable,g_bind_last.header.magic,g_bind_last.header.nstage,(unsigned long long)g_bind_last.header.serial);
}
static void bind_diagnostics_configure(void) {
    const char *env=getenv("DM_NVN_BIND_DIAGNOSTICS");g_bind_diagnostics=env&&atoi(env);
    if(g_bind_diagnostics)crash_context_dump_register(bind_diagnostics_dump);
}
static void bind_diagnostics_init(void) {static pthread_once_t once=PTHREAD_ONCE_INIT;pthread_once(&once,bind_diagnostics_configure);}
static void bind_diagnostics_record(const void *command,const p_bind_program *packet) {
    if(!g_bind_diagnostics)return;
    g_bind_last.sequence++;g_bind_last.command=command;g_bind_last.program=packet->prog;g_bind_last.stages=packet->stages;
    memset(&g_bind_last.header,0,sizeof g_bind_last.header);vm_size_t got=0;
    g_bind_last.readable=packet->prog&&vm_read_overwrite(mach_task_self(),(vm_address_t)packet->prog,sizeof g_bind_last.header,(vm_address_t)&g_bind_last.header,&got)==KERN_SUCCESS&&got==sizeof g_bind_last.header;
}
