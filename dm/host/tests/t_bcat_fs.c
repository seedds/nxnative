#include "../hle_fs.c"
#include <stdio.h>
struct dm_flags g_flag;uint64_t g_t0_ns;uint64_t now_ns(void){return 0;}
void dm_guest_write_prepare(void *p,size_t n){(void)p;(void)n;}
extern char bcat_fixture_overlay[1024],bcat_fixture_delivery[1024];
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"BCAT FS failed line%d: %s\n",__LINE__,#c);return 1;}}while(0)
static void put(const char *p,const char *s){FILE *f=fopen(p,"w");if(!f)abort();fwrite(s,1,strlen(s),f);fclose(f);}
int main(void){char root[]="/tmp/nx-bcat-fs-XXXXXX",base[1024],overlay[1024],delivery[1024],p[1200];CHECK(mkdtemp(root));snprintf(base,sizeof base,"%s/base",root);snprintf(overlay,sizeof overlay,"%s/overlay",root);snprintf(delivery,sizeof delivery,"%s/delivery",root);CHECK(!mkdir(base,0700)&&!mkdir(overlay,0700)&&!mkdir(delivery,0700));snprintf(p,sizeof p,"%s/base.txt",base);put(p,"base");snprintf(p,sizeof p,"%s/calendar.txt",overlay);put(p,"calendar");snprintf(p,sizeof p,"%s/event.txt",delivery);put(p,"event");
    CHECK(h_bcat_Mount()!=RES_OK);strlcpy(bcat_fixture_overlay,overlay,1024);strlcpy(bcat_fixture_delivery,delivery,1024);setenv("DM_ROMFS",base,1);CHECK(!h_fs_MountRom("content",NULL,0));
    uint64_t handle;size_t n;char bytes[16]={0};CHECK(!h_fs_OpenFile(&handle,"content:/calendar.txt",MODE_READ));CHECK(!h_fs_ReadFile(&n,handle,0,bytes,8)&&n==8&&!memcmp(bytes,"calendar",8));h_fs_CloseFile(handle);
    CHECK(!h_fs_OpenFile(&handle,"content:/base.txt",MODE_READ));h_fs_CloseFile(handle);
    CHECK(h_fs_OpenFile(&handle,"content:/calendar.txt",MODE_WRITE)!=0);
    uint64_t dir;CHECK(!h_fs_OpenDirectory(&dir,"content:/",DIRMODE_FILE));DirectoryEntry entries[4];int64_t count;CHECK(!h_fs_ReadDirectory(&count,entries,dir,4)&&count==2);h_fs_CloseDirectory(dir);
    CHECK(!h_bcat_Mount());CHECK(!h_fs_OpenFile(&handle,"bcat:/event.txt",MODE_READ));h_bcat_Unmount();uint64_t other;CHECK(h_fs_OpenFile(&other,"bcat:/event.txt",MODE_READ)!=0);CHECK(!h_fs_ReadFile(&n,handle,0,bytes,5)&&n==5&&!memcmp(bytes,"event",5));h_fs_CloseFile(handle);
    CHECK(!h_bcat_Mount());CHECK(!h_fs_OpenFile(&handle,"bcat:/event.txt",MODE_READ));h_fs_CloseFile(handle);CHECK(h_fs_OpenFile(&handle,"bcat:/event.txt",MODE_WRITE)!=0);h_bcat_Unmount();
    for(const char **name=(const char *[]){"base/base.txt","overlay/calendar.txt","delivery/event.txt",NULL};*name;name++){snprintf(p,sizeof p,"%s/%s",root,*name);CHECK(!unlink(p));}CHECK(!rmdir(base)&&!rmdir(overlay)&&!rmdir(delivery)&&!rmdir(root));printf("BCAT RomFS merge/read-only mount/unmount: %u checks passed\n",checks);return 0;
}
