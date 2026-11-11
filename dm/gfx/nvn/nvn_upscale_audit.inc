// Opt-in metadata only: report every bound input/output of the live scene chain.
static uint64_t g_ua_first=UINT64_MAX;
static const char *g_ua_out,*g_ua_arm;
static int g_ua_on,g_ua_done;
static NSMutableArray *g_ua_rows;
static void upscale_audit_frame(uint64_t frame) {
    static int configured;
    if(!configured){configured=1;g_ua_out=getenv("DM_NVN_UPSCALE_AUDIT_OUT");g_ua_arm=getenv("DM_NVN_UPSCALE_AUDIT_ARM");g_ua_on=g_ua_out&&*g_ua_out&&g_ua_arm&&*g_ua_arm;}
    if(!g_ua_on||g_ua_done)return;
    if(g_ua_first==UINT64_MAX){FILE *f=fopen(g_ua_arm,"r");if(!f)return;fclose(f);g_ua_first=frame;g_ua_rows=[NSMutableArray array];return;}
    if(frame<g_ua_first+3)return;
    NSError *error=nil;NSDictionary *result=@{@"first_frame":@(g_ua_first),@"frames":@3,@"rows":g_ua_rows?:@[],@"scope":@"binding metadata; no buffer-content or last-writer proof"};
    NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:&error];
    int ok=json&&[json writeToFile:@(g_ua_out) options:NSDataWritingAtomic error:&error];
    MLOG("%sUPSCALE_AUDIT_DONE rows %lu ok=%d",ok?"":"[RED] ",(unsigned long)g_ua_rows.count,ok);g_ua_done=1;g_ua_rows=nil;
}
// Caller holds g_rmu; the dispatch has already resolved its argument buffer.
static void upscale_audit_dispatch(NvShader *cs) {
    uint64_t frame=atomic_load(&g_cur_frame);
    if(!g_ua_on||g_ua_done||g_ua_first==UINT64_MAX||frame<g_ua_first||frame>=g_ua_first+3)return;
    if(g_ua_rows.count>=64)return;
    NSMutableArray *bindings=[NSMutableArray array];
    size_t length=cs->arg_len?cs->arg_len:cs->ae.encodedLength;
    const uint64_t *args=cs->ab_buf&&cs->ab_off+length<=cs->ab_buf.length?(const void *)((const uint8_t *)cs->ab_buf.contents+cs->ab_off):NULL;
    for(int i=0;i<cs->rf.nb;i++){const nvn_rbind *b=&cs->rf.b[i];NSMutableDictionary *entry=[@{@"kind":@(b->kind),@"reg":@(b->reg),@"arg_id":@(b->aid),@"write":@(b->rw),@"atomic":@(b->atom)} mutableCopy];
        if(args&&(b->aid+1)*sizeof(uint64_t)<=length)entry[@"argument_value"]=@(args[b->aid]);
        int id_=0;if(b->kind==NVN_RB_TEX)id_=(int)(tex_slot_h(S.tex[5],S.img[5],b->reg)&0xfffff);
        else if(b->kind==NVN_RB_IMG&&b->reg<32)id_=(int)(S.img[5][b->reg]&0xfffff);
        if(id_>0&&id_<g_ntreg&&g_treg[id_].ok){const tex_t *t=&g_treg[id_].t;
            entry[@"texture"]=@{@"id":@(id_),@"serial":@(t->serial),@"memory":@((uint64_t)(uintptr_t)t->mem),@"storage_bytes":@(t->L.size),@"width":@(t->d.width),@"height":@(t->d.height),@"format":@(t->d.format)};}
        if(b->reg<16){const p_bind_buf *u=&S.ubo[5][b->reg],*s=&S.ssbo[5][b->reg];entry[@"uniform_binding"]=@{@"address":@(u->addr),@"size":@(u->size)};entry[@"storage_binding"]=@{@"address":@(s->addr),@"size":@(s->size)};}
        [bindings addObject:entry];}
    NSData *push=[NSData dataWithBytes:S.push[1] length:DMSH_PUSH_SIZE];
    [g_ua_rows addObject:@{@"frame":@(frame),@"shader":@(cs->key),@"scene_serial":@(g_temporal_flow[0]),@"bindings":bindings,@"push_base64":[push base64EncodedStringWithOptions:0]}];
}
