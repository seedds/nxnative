// Original SDK-facing delivery cache. File/directory objects own real immutable
// file handles; synchronization progress reflects the actual public downloader.
#include "hle.h"
#include "nextendo_bcat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char name[32]; int64_t size; unsigned char digest[16]; } BcatEntry;
typedef struct { uint64_t lo,hi; } BcatDigest;
_Static_assert(sizeof(BcatEntry)==0x38,"SDK delivery directory entry");
_Static_assert(sizeof(BcatDigest)==16,"AAPCS64 digest returns in x0/x1");
enum { BCAT_FILE=1,BCAT_DIRECTORY=2,BCAT_PROGRESS=3,BCAT_OBJECTS=128 };
typedef struct {
    void *object;int kind,fd;BcatEntry entries[64];int count,cursor,opened;
    BcatDigest digest;unsigned status,result;uint64_t downloaded;void *observer;
} BcatObject;
static BcatObject objects[BCAT_OBJECTS];
static pthread_mutex_t bcatLock=PTHREAD_MUTEX_INITIALIZER;
static BcatObject *find(void *object,int kind){for(int i=0;i<BCAT_OBJECTS;i++)if(objects[i].object==object&&objects[i].kind==kind)return &objects[i];return NULL;}
static void release(BcatObject *o){if(!o)return;if(o->fd>=0)close(o->fd);if(o->observer)dm_bcat_sync_release(o->observer);memset(o,0,sizeof *o);o->fd=-1;}
static void create(void *object,int kind){
    if(!object)return;pthread_mutex_lock(&bcatLock);
    for(int i=0;i<BCAT_OBJECTS;i++)if(objects[i].object==object){release(&objects[i]);break;}
    for(int i=0;i<BCAT_OBJECTS;i++)if(!objects[i].object){objects[i]=(BcatObject){.object=object,.kind=kind,.fd=-1,.result=kind==BCAT_PROGRESS?RES_OK:RES_FAIL};break;}
    // The SDK 3 public constructors initialize this 8-byte implementation slot.
    // Our storage stays host-owned and no SDK mutex/private allocation is used.
    *(uint64_t *)object=0;pthread_mutex_unlock(&bcatLock);
}
static void destroy(void *object,int kind){pthread_mutex_lock(&bcatLock);release(find(object,kind));pthread_mutex_unlock(&bcatLock);}
static int component(const char *s){if(!s)return 0;size_t n=strnlen(s,32);if(!n||n==32||!strcmp(s,".")||!strcmp(s,".."))return 0;for(size_t i=0;i<n;i++)if((unsigned char)s[i]<32||(unsigned char)s[i]>126||s[i]=='/'||s[i]=='\\'||s[i]==':')return 0;return 1;}
static int directory_fd(const char *directory){char root[1024];if(!component(directory)||!dm_bcat_copy_delivery(root,sizeof root))return -1;int base=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(base<0)return -1;int fd=openat(base,directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(base);return fd;}
static int file_digest(int fd,BcatDigest *digest,int64_t *size){struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>8*1024*1024)return 0;size_t length=(size_t)st.st_size;void *bytes=malloc(length?length:1);if(!bytes)return 0;size_t done=0;while(done<length){ssize_t n=pread(fd,(char *)bytes+done,length-done,(off_t)done);if(n<=0){free(bytes);return 0;}done+=(size_t)n;}dm_bcat_digest(bytes,length,(unsigned char *)digest);free(bytes);if(size)*size=st.st_size;return 1;}
static int compare_entries(const void *a,const void *b){return strcmp(((const BcatEntry *)a)->name,((const BcatEntry *)b)->name);}

HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFileC1Ev","construct host-owned read-only delivery file",void,h_bcat_FileCtor,void *object){create(object,BCAT_FILE);}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFileC2Ev","construct host-owned read-only delivery file",void,h_bcat_FileCtor2,void *object){create(object,BCAT_FILE);}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFileD1Ev","close delivery file",void,h_bcat_FileDtor,void *object){destroy(object,BCAT_FILE);}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFileD2Ev","close delivery file",void,h_bcat_FileDtor2,void *object){destroy(object,BCAT_FILE);}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile5CloseEv","close a delivery file while keeping its reusable object",void,h_bcat_FileClose,void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_FILE);if(o){if(o->fd>=0)close(o->fd);o->fd=-1;o->digest=(BcatDigest){0};}pthread_mutex_unlock(&bcatLock);}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE","open a real file in the verified immutable delivery snapshot",Result,h_bcat_FileOpen,void *object,const char *directory,const char *name){
    if(!component(name))return RES_FAIL;int parent=directory_fd(directory);if(parent<0)return RES_FAIL;int fd=openat(parent,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);close(parent);if(fd<0)return RES_FAIL;
    BcatDigest digest;if(!file_digest(fd,&digest,NULL)){close(fd);return RES_FAIL;}
    pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_FILE);if(o){if(o->fd>=0)close(o->fd);o->fd=fd;o->digest=digest;}else close(fd);pthread_mutex_unlock(&bcatLock);return o?RES_OK:RES_FAIL;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm","bounded positional read from a real delivery file",Result,h_bcat_FileRead,void *object,uint64_t *out,int64_t offset,void *buffer,size_t size){
    if(!out||offset<0||(!buffer&&size)||size>INT64_MAX)return RES_FAIL;*out=0;pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_FILE);ssize_t n=-1;if(o&&o->fd>=0)n=pread(o->fd,buffer,size,(off_t)offset);if(n>=0)*out=(uint64_t)n;pthread_mutex_unlock(&bcatLock);return n>=0?RES_OK:RES_FAIL;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile9GetDigestEv","MD5 of the opened delivery bytes, returned as the SDK 16-byte value",BcatDigest,h_bcat_FileDigest,void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_FILE);int opened=o&&o->fd>=0;BcatDigest result=opened?o->digest:(BcatDigest){0};pthread_mutex_unlock(&bcatLock);if(!opened)die("BCAT GetDigest requires an opened delivery file");return result;}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile7GetSizeEv","size of the opened delivery file",int64_t,h_bcat_FileSize,void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_FILE);struct stat st;int opened=o&&o->fd>=0&&!fstat(o->fd,&st);int64_t size=opened?st.st_size:0;pthread_mutex_unlock(&bcatLock);if(!opened)die("BCAT GetSize requires an opened delivery file");return size;}

HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev","construct delivery directory",void,h_bcat_DirCtor,void *object){create(object,BCAT_DIRECTORY);}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectoryC2Ev","construct delivery directory",void,h_bcat_DirCtor2,void *object){create(object,BCAT_DIRECTORY);}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev","release directory snapshot",void,h_bcat_DirDtor,void *object){destroy(object,BCAT_DIRECTORY);}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectoryD2Ev","release directory snapshot",void,h_bcat_DirDtor2,void *object){destroy(object,BCAT_DIRECTORY);}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv","close the opened directory snapshot",void,h_bcat_DirClose,void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_DIRECTORY);if(o){o->count=o->cursor=o->opened=0;}pthread_mutex_unlock(&bcatLock);}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory8GetCountEv","count of entries in the opened real directory snapshot",int,h_bcat_DirCount,void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_DIRECTORY);int opened=o&&o->opened;int count=opened?o->count:0;pthread_mutex_unlock(&bcatLock);if(!opened)die("BCAT GetCount requires an opened delivery directory");return count;}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE","snapshot real delivery entries, sizes and digests",Result,h_bcat_DirOpen,void *object,const char *directory){
    int fd=directory_fd(directory);if(fd<0)return RES_FAIL;DIR *dir=fdopendir(fd);if(!dir){close(fd);return RES_FAIL;}
    BcatEntry entries[64];int count=0,ok=1;struct dirent *entry;
    while((entry=readdir(dir))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;if(!component(entry->d_name)||count==64){ok=0;break;}
        int file=openat(fd,entry->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);BcatDigest digest;int64_t size;if(file<0||!file_digest(file,&digest,&size)){if(file>=0)close(file);ok=0;break;}close(file);
        BcatEntry *e=&entries[count++];memset(e,0,sizeof *e);strlcpy(e->name,entry->d_name,sizeof e->name);e->size=size;memcpy(e->digest,&digest,16);}
    closedir(dir);if(!ok)return RES_FAIL;qsort(entries,(size_t)count,sizeof entries[0],compare_entries);
    pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_DIRECTORY);if(o){memcpy(o->entries,entries,(size_t)count*sizeof entries[0]);o->count=count;o->cursor=0;o->opened=1;}pthread_mutex_unlock(&bcatLock);return o?RES_OK:RES_FAIL;
}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi","bounded enumeration of the opened directory snapshot",Result,h_bcat_DirRead,void *object,int *out,BcatEntry *entries,int capacity){
    if(!out||capacity<0||(!entries&&capacity))return RES_FAIL;*out=0;
    pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_DIRECTORY);
    if(!o||!o->opened){pthread_mutex_unlock(&bcatLock);return RES_FAIL;}
    int n=o->count-o->cursor;
    if(n<0){pthread_mutex_unlock(&bcatLock);return RES_FAIL;}
    if(n>capacity)n=capacity;
    if(n>0){
        if(!entries){pthread_mutex_unlock(&bcatLock);return RES_FAIL;}
        memcpy(entries,o->entries+o->cursor,(size_t)n*sizeof *entries);
    }
    o->cursor+=n;*out=n;pthread_mutex_unlock(&bcatLock);return RES_OK;
}
HLE_FORCE("_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi","enumerate real delivery directories",Result,h_bcat_Enumerate,int *out,char (*names)[32],int capacity){
    if(!out||capacity<0||(!names&&capacity))return RES_FAIL;*out=0;char root[1024];if(!dm_bcat_copy_delivery(root,sizeof root))return RES_FAIL;DIR *dir=opendir(root);if(!dir)return RES_FAIL;struct dirent *e;int total=0;char found[64][32];
    while((e=readdir(dir))){struct stat st;if(!component(e->d_name))continue;if(fstatat(dirfd(dir),e->d_name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(st.st_mode))continue;if(total==64){closedir(dir);return RES_FAIL;}strlcpy(found[total++],e->d_name,32);}closedir(dir);qsort(found,(size_t)total,32,(int(*)(const void*,const void*))strcmp);int n=total<capacity?total:capacity;if(n)memcpy(names,found,(size_t)n*32);*out=n;return RES_OK;
}
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgressC1Ev","construct synchronization progress",void,h_bcat_ProgressCtor,void *object){create(object,BCAT_PROGRESS);}
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgressC2Ev","construct synchronization progress",void,h_bcat_ProgressCtor2,void *object){create(object,BCAT_PROGRESS);}
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgressD1Ev","release progress observer",void,h_bcat_ProgressDtor,void *object){destroy(object,BCAT_PROGRESS);}
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgressD2Ev","release progress observer",void,h_bcat_ProgressDtor2,void *object){destroy(object,BCAT_PROGRESS);}
static void progress_update(BcatObject *o){unsigned result=1;uint64_t downloaded=0;o->status=(unsigned)dm_bcat_sync_observer_status(o->observer,&result,&downloaded);o->result=o->status==9&&result?RES_FAIL:RES_OK;o->downloaded=downloaded;}
HLE_FORCE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE","observe a real background HTTPS calendar refresh",Result,h_bcat_Sync,void *object){
    pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_PROGRESS);void *observer=o?dm_bcat_sync_observe():NULL;
    // SDK 3 attaches a successful request by releasing the previous service and
    // replacing its state. A failed request leaves that observer untouched.
    if(observer){if(o->observer)dm_bcat_sync_release(o->observer);o->observer=observer;progress_update(o);}
    pthread_mutex_unlock(&bcatLock);return observer?RES_OK:RES_FAIL;
}
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv","copy actual synchronization state",void,h_bcat_ProgressUpdate,void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find(object,BCAT_PROGRESS);if(o&&o->observer)progress_update(o);pthread_mutex_unlock(&bcatLock);}
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv","SDK status: zero before request, connecting or done9 after real completion",unsigned,h_bcat_ProgressStatus,const void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find((void*)object,BCAT_PROGRESS);unsigned status=o?o->status:0;pthread_mutex_unlock(&bcatLock);return status;}
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv","final validation/download result; pending is not completion (status must be done9)",Result,h_bcat_ProgressResult,const void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find((void*)object,BCAT_PROGRESS);Result result=o?o->result:RES_FAIL;pthread_mutex_unlock(&bcatLock);return result;}
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress18GetWholeDownloadedEv","validated delivery bytes after a completed refresh",uint64_t,h_bcat_ProgressDownloaded,const void *object){pthread_mutex_lock(&bcatLock);BcatObject *o=find((void*)object,BCAT_PROGRESS);uint64_t n=o?o->downloaded:0;pthread_mutex_unlock(&bcatLock);return n;}
