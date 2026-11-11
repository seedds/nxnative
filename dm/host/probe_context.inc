// Opt-in postmortem reads. Never patch guest instructions or dereference a guest
// address directly. Syntax: root [+/-hex | *]... : hex-byte-count; comma separated.
// roots: x0..x28, fp, sp, pc, lr, or an exact loaded module name.
void crash_context_dump_register(void (*fn)(const void *));
static int context_read(uint64_t address,void *out,size_t size) {
    vm_size_t got=0;
    return address&&vm_read_overwrite(mach_task_self(),(vm_address_t)address,size,(vm_address_t)out,&got)==KERN_SUCCESS&&got==size;
}
static int context_expression(const char *spec,const ucontext_t *uc,uint64_t *address,size_t *length) {
    char root[32];size_t n=strcspn(spec,"+-*:");if(!n||n>=sizeof root)return 0;
    memcpy(root,spec,n);root[n]=0;uint64_t at=0;int found=1;
    if(!strcmp(root,"sp"))at=uc->uc_mcontext->__ss.__sp;
    else if(!strcmp(root,"fp"))at=uc->uc_mcontext->__ss.__fp;
    else if(!strcmp(root,"pc"))at=uc->uc_mcontext->__ss.__pc;
    else if(!strcmp(root,"lr"))at=uc->uc_mcontext->__ss.__lr;
    else if(root[0]=='x') {char *end;long reg=strtol(root+1,&end,10);if(!root[1]||*end||reg<0||reg>28)return 0;at=uc->uc_mcontext->__ss.__x[reg];}
    else {found=0;for(int i=0;i<g_nmod;i++)if(!strcmp(root,g_mod[i].name)){at=(uint64_t)(uintptr_t)g_mod[i].base;found=1;break;}}
    if(!found)return 0;
    const char *q=spec+n;unsigned operations=0;
    while(*q&&*q!=':') {
        if(++operations>16)return 0;
        if(*q=='*') {if(!context_read(at,&at,sizeof at))return 0;q++;continue;}
        int subtract=*q=='-';if(!subtract&&*q!='+')return 0;
        char *end;const char *start=++q;if(!((*q>='0'&&*q<='9')||(*q>='a'&&*q<='f')||(*q>='A'&&*q<='F')))return 0;
        unsigned long long offset=strtoull(start,&end,16);if(end==start)return 0;
        if(subtract){if(offset>at)return 0;at-=offset;}else{if(offset>UINT64_MAX-at)return 0;at+=offset;}q=end;
    }
    if(*q++!=':')return 0;char *end;unsigned long count=strtoul(q,&end,16);
    if(end==q||*end||!count||count>256)return 0;
    *address=at;*length=count;return 1;
}
static void probe_context_dump(const void *context) {
    const char *env=getenv("DM_CRASH_READS");if(!env||!*env)return;
    char specs[2048];if(strlen(env)>=sizeof specs){fprintf(stderr,"[dm] CRASH_READ specification too long\n");return;}
    memcpy(specs,env,strlen(env)+1);char *save=NULL;unsigned count=0;
    for(char *spec=strtok_r(specs,",",&save);spec&&count++<16;spec=strtok_r(NULL,",",&save)) {
        uint64_t address=0;size_t length=0;uint8_t bytes[256];
        if(!context_expression(spec,context,&address,&length)||!context_read(address,bytes,length)) {fprintf(stderr,"[dm] CRASH_READ %s unavailable\n",spec);continue;}
        fprintf(stderr,"[dm] CRASH_READ %s address=%#llx data=",spec,(unsigned long long)address);
        for(size_t i=0;i<length;i++)fprintf(stderr,"%02x",bytes[i]);
        module_t *module=module_of((void *)(uintptr_t)address);Dl_info info;
        if(module)fprintf(stderr," location=%s+%#llx",module->name,(unsigned long long)(address-(uint64_t)(uintptr_t)module->base));
        else if(dladdr((void *)(uintptr_t)address,&info)&&info.dli_sname)fprintf(stderr," symbol=%s",info.dli_sname);
        fputc('\n',stderr);
    }
}
static void probe_context_install(void) {if(getenv("DM_CRASH_READS"))crash_context_dump_register(probe_context_dump);}
