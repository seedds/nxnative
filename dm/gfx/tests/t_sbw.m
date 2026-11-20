// Other GPU writes of a submission (sbw_note / sbw_hits): a storage buffer bound with size 0 counts as the rest of its Metal buffer, kept as an interval,
// so a uniform block in another buffer can still be read from memory (GitHub issue #534: Breath of the Wild's shrines)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
int main(void){@autoreleasepool{
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    id<MTLBuffer> pool=[dev newBufferWithLength:0xa04000 options:MTLResourceStorageModeShared];   // BotW's particle pool: 10 MB at 0x70c5d50000
    void *mb=(__bridge void*)pool;const uint64_t base=0x70c5d50000ull,ub=0x70c38c0000ull;
    CHECK("a sized binding writes its size",sbw_len(64,mb,0x40d80)==64);
    CHECK("a size-0 binding writes the rest of its buffer",sbw_len(0,mb,0x40d80)==0xa04000-0x40d80);
    CHECK("an offset past the buffer writes nothing",sbw_len(0,mb,0xa04000)==0);
    pend_reset();
    for(int s=0;s<3;s++){uint64_t o=0x40d80+0x400*s;sbw_note(base+o,sbw_len(0,mb,o),SBW_GPU);}   // the three outputs of cs_effect var3 (9f8116baffddaf81)
    CHECK("size-0 bindings do not stop the tracking",!g_sbw_over);
    CHECK("nested wide writes merge into one interval",g_nsbw_w==1);
    CHECK("the uniform block in another buffer is not hit",!sbw_hits(ub,ub+0x100));
    CHECK("the particle outputs are hit",sbw_hits(base+0x41000,base+0x41010));
    CHECK("the rest of their buffer is hit",sbw_hits(base+0xa03ff0,base+0xa04000));
    CHECK("bytes before the first output are not hit",!sbw_hits(base,base+0x40d80));
    sbw_note(ub+0x2000,16,SBW_GPU);
    CHECK("a small write is still kept by page",sbw_hits(ub+0x2f00,ub+0x2f10)&&!sbw_hits(ub,ub+0x100));
    sbw_note(ub,0,SBW_GPU);
    CHECK("a write of no known size stops the tracking",g_sbw_over);
    pend_reset();
    CHECK("a new submission starts empty",!g_sbw_over&&!g_nsbw_w&&!sbw_hits(base+0x41000,base+0x41010));
    for(int i=0;i<SBW_WN;i++)sbw_note(base+(uint64_t)i*0x100000,0x80000,SBW_GPU);
    CHECK("disjoint wide writes each take an interval",!g_sbw_over&&g_nsbw_w==SBW_WN);
    sbw_note(base+(uint64_t)SBW_WN*0x100000,0x80000,SBW_GPU);
    CHECK("one more than the list holds stops the tracking",g_sbw_over);
    pend_reset();sbw_note(base+0x40d80,sbw_len(0,mb,0x40d80),SBW_GPU);
    uint64_t done=g_done;gw_commit(done+1000);
    CHECK("a running submission's wide write is seen by the next",gw_live(base+0x41000,base+0x41010));
    CHECK("and not elsewhere",!gw_live(ub,ub+0x100));
    // The commit reads the pages this submission noted, not the whole page set (Ocarina of Time commits about 30 submissions a frame): a GPU page goes into the running writes,
    // a page only a report wrote does not (the report counts by its own 16 bytes), and a page of an earlier submission is not met again
    pend_reset();{const uint64_t a=0x7300000000ull,b=0x7300100000ull,r=0x7300200000ull;
    sbw_note(a,16,SBW_GPU);sbw_note(b+0x10,0x1800,SBW_GPU);sbw_note(r,16,SBW_REPORT);sbw_note(a+8,8,SBW_GPU);
    CHECK("the submission lists each page it noted once",g_nsbw==4&&g_sbwp[g_sbw_list[0]].page==a>>12&&g_sbwp[g_sbw_list[1]].page==b>>12&&g_sbwp[g_sbw_list[2]].page==(b>>12)+1&&g_sbwp[g_sbw_list[3]].page==r>>12);
    gw_commit(done+900);
    CHECK("its GPU pages are running writes",gw_live(a+0x800,a+0x810)&&gw_live(b+0x1000,b+0x1010));
    CHECK("a page only a report wrote is not",!gw_live_in(g_gw,r+0x100,r+0x110));
    pend_reset();sbw_note(ub+0x3000,16,SBW_GPU);
    CHECK("the next submission lists only its own pages",g_nsbw==1&&g_sbwp[g_sbw_list[0]].page==(ub+0x3000)>>12);}
    // A write of 16 MB or more is kept whole: a uniform block elsewhere stays readable, and the same range coming back takes no new slot
    pend_reset();uint64_t wide=0x7100000000ull,big=0x2000000;
    sbw_note(wide,big,SBW_GPU);gw_commit(done+1001);
    CHECK("a running 32 MB write is seen inside",gw_live(wide+0x1000000,wide+0x1000010));
    CHECK("and not outside",!gw_live(wide+big,wide+big+16)&&!gw_live(ub,ub+0x100));
    CHECK("and is not a write anywhere",g_gw_all<=done);
    pend_reset();for(int i=0;i<GWW_N*2;i++){sbw_note(wide,big,SBW_GPU);gw_commit(done+1002+i);pend_reset();}
    CHECK("the same range every submission stays one write",g_gw_all<=done&&!gw_live(ub,ub+0x100));
    for(int i=0;i<GWW_N;i++){sbw_note(wide+(uint64_t)(i+1)*0x10000000,big,SBW_GPU);gw_commit(done+2000);pend_reset();}
    CHECK("more live wide writes than the list holds count as a write anywhere",gw_live(ub,ub+0x100));
    // Conditional rendering: however many query reports come first, a timestamp report is remembered, and a condition on a query stays unknown
    {static uint64_t rec[2]={1,0},q[4];uint64_t x=~0ull;
    for(uint64_t i=0;i<9000;i++)rc_note(0x7100000000ull+i*16,2);
    rc_note((uint64_t)&rec[0],1);
    CHECK("a timestamp report after 9000 query reports counts as 0",rc_value((uint64_t)&rec[0],&x)&&x==0);
    CHECK("a record with no report is unknown while a GPU write anywhere still runs (above)",!rc_value((uint64_t)&rec[1],&x));
    uint64_t was=g_done;g_done=done+100000;   // every write above has landed
    CHECK("once nothing that could write it runs, a record with no report reads memory",rc_value((uint64_t)&rec[1],&x)&&x==0);
    g_done=was;
    CHECK("a query's report stays unknown",!rc_value(0x7100000000ull+8999*16,&x)&&!rc_value(0x7100000000ull,&x));
    (void)q;for(uint64_t i=0;i<300;i++)rc_note((uint64_t)&big+0x1000+i,1);
    CHECK("more timestamp reports than the first table held",rc_value((uint64_t)&big+0x1000,&x)&&x==0);
    rc_forget();}
    // Sparse report pages arrive out of address order. A large write must visit
    // just its intersections, including an unaligned counter on the prior page.
    {const uint64_t start=0x7200100000ull, span=0x800000, addresses[]={start+span,start-3,start+0x700000,start-16,start+32,start+0x100000};
    const uint8_t fill=0xab;uint64_t x=0;
    for(unsigned i=0;i<sizeof addresses/sizeof *addresses;i++)rc_note(addresses[i],1);
    rc_write(start,span,&fill,1);
    CHECK("wide write excludes the counter at its end",rc_value(addresses[0],&x)&&x==0);
    CHECK("wide write forwards partial counter bytes across a page",rc_value(addresses[1],&x)&&x==0xababababab000000ull);
    CHECK("wide write reaches distant intersecting report pages",rc_value(addresses[2],&x)&&x==0xababababababababull);
    CHECK("wide write excludes counters before its start",rc_value(addresses[3],&x)&&x==0);
    CHECK("wide write reaches another counter on its first page",rc_value(addresses[4],&x)&&x==0xababababababababull);
    CHECK("wide write reaches a page inserted last",rc_value(addresses[5],&x)&&x==0xababababababababull);
    rc_forget();rc_note(start+32,1);rc_write(start,span,&fill,1);
    CHECK("report page index resets before address reuse",rc_value(start+32,&x)&&x==0xababababababababull);
    rc_forget();}
    // An unbounded shader store may replace a completed report before the word
    // becomes a query operand. Copy forwarding must not read its stale CPU bytes.
    {static uint64_t rec[2];uint8_t bytes[8];int unreliable=0;uint64_t x=1;
    uint64_t completed=atomic_load(&g_rc_complete);rc_note((uint64_t)rec,1);rc_commit(completed+1);atomic_store(&g_rc_complete,completed+1);
    sbw_note(0,0,SBW_GPU);
    CHECK("a copy after an unbounded writer cannot forward completed timestamp bytes",!rc_source((uint64_t)rec,8,bytes,&unreliable)&&unreliable);
    rc_note((uint64_t)rec,1);
    CHECK("a later timestamp restores known zero after the opaque writer",rc_value((uint64_t)rec,&x)&&x==0);
    rc_forget();atomic_store(&g_rc_complete,completed);}
    pend_reset();
    printf("t_sbw: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
