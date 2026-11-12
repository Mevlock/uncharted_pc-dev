/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <ftw.h>
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used, writable; char host[700], meta[700]; } slots[SLOTS];
static char memory_path[720];
static uint64_t memory_size;
static size_t mounts_done, memory_writes;

/* Bloodborne "sound hack" (rainvmaker, from the Diegolix29 shadPS4 fork): the game data
 * save carries a flag at 0x204E of userdata0010; with it clear, parts of the game's audio
 * (e.g. the player's weapon sounds) never play. It is set before the game reads the save.
 * BB_SOUND_HACK=0 leaves saves untouched. */
static void bloodborne_sound_hack(void) {
    static const char *const ids[]={"CUSA00207","CUSA00208","CUSA00299","CUSA00900","CUSA01363","CUSA03014","CUSA03023","CUSA03173"};
    const char *env=getenv("BB_SOUND_HACK");
    if (env && env[0]=='0') return;
    int bloodborne=0;
    for (size_t i=0;i<sizeof(ids)/sizeof(*ids);++i) if (!strcmp(title_id,ids[i])) bloodborne=1;
    if (!bloodborne) return;
    char users[700];
    snprintf(users,sizeof(users),"%s/savedata",runtime_file_user_dir());
    DIR *dir=opendir(users);
    if (!dir) return;
    for (struct dirent *e; (e=readdir(dir));) {
        if (e->d_name[0]=='.') continue;
        char path[1100];
        snprintf(path,sizeof(path),"%s/%s/%s/SPRJ0005/userdata0010",users,e->d_name,title_id);
        FILE *f=fopen(path,"r+b");
        if (!f) continue;
        int old=fseek(f,0x204E,SEEK_SET) ? EOF : fgetc(f);
        if (old!=EOF && old!=1 && !fseek(f,0x204E,SEEK_SET)) {
            fputc(1,f);
            printf("Runtime: Bloodborne sound flag set in %s (was %d)\n",path,old);
        }
        fclose(f);
    }
    closedir(dir);
}
void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
    bloodborne_sound_hack();
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
/* A whole file replaced in one rename (a crash mid-write keeps the old one). */
static int write_atomic(const char *path, const void *data, size_t size) {
    char temp[760]; snprintf(temp,sizeof(temp),"%s.bbtmp",path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fwrite(data,1,size,f)==size && !fflush(f) && !fsync(fileno(f));
    ok=!fclose(f) && ok;
    if (!ok || runtime_file_replace(temp,path)) { unlink(temp); return -1; }
    return 0;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    Param copy=*p; copy.mtime=time(NULL);
    write_atomic(path,&copy,sizeof(copy));
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
#ifdef _WIN32
static void remove_tree(const char *path) { compat_remove_tree(path); }
#else
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
static void remove_tree(const char *path) { nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
#endif

/* Save auto-repair (the idea from BilalReloaded's port 0.8.0, his #311: a power cut while saving).
 * Each time the game closes a save, every file in it that holds data (not empty, not all zero bytes)
 * is copied to <title>.backups/auto/<dir name>/; a file that is empty or all zeros is never copied,
 * so a good backup is never replaced by a broken file. The close also writes <dir name>/.closed: per
 * file whether that close left it with data ("D <path>") or empty or all zeros ("E <path>").
 * When the game opens a save, a file in it that is empty or all zeros while its backup holds data is
 * put back from the backup (logged) only if the last close left it with data: it was broken outside a
 * close (a power cut or a crash while saving), not emptied by the game itself (backups from builds
 * without .closed: repaired as before). Files are only repaired, never added or removed, and a file
 * with data is never overwritten. A save the game deletes moves its backup to <dir name>.deleted, and
 * a save whose folder is gone when the game opens or creates it (a new save, or one deleted outside the
 * game) moves it to <dir name>.stale: a new save of that name is not "repaired" from the old one.
 * The practice copy of checkpoint jumps is left alone. BB_SAVE_BACKUP=0: off. */
#define BACKUP_MAX_FILE (256u<<20)
static pthread_mutex_t backup_lock=PTHREAD_MUTEX_INITIALIZER;
static int backup_enabled(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_SAVE_BACKUP"); on=!(e && e[0]=='0'); }
    return on;
}
/* A whole file (NULL: missing, unreadable or too big); *size its length. */
static unsigned char *read_whole(const char *path, size_t *size) {
    *size=0;
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    unsigned char *data=NULL;
    long n=fseek(f,0,SEEK_END) ? -1 : ftell(f);
    if (n>=0 && (unsigned long)n<=BACKUP_MAX_FILE && !fseek(f,0,SEEK_SET)) {
        data=malloc(n ? (size_t)n : 1);
        if (data && fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); data=NULL; }
        if (data) *size=(size_t)n;
    }
    fclose(f);
    return data;
}
static int holds_data(const unsigned char *data, size_t size) {
    for (size_t i=0;data && i<size;++i) if (data[i]) return 1;
    return 0;
}
static int is_temp_name(const char *name) {
    const size_t n=strlen(name);
    return name[0]=='.' || (n>6 && !strcmp(name+n-6,".bbtmp"));
}
/* The last close's state of a save's files (.closed in its backup): a text of "D <path>\n" / "E <path>\n" lines. */
typedef struct { char *text; size_t size, capacity; } Closed;
static void closed_add(Closed *c, char kind, const char *rel) {
    const size_t n=strlen(rel)+3;
    if (c->size+n+1>c->capacity) {
        size_t capacity=c->capacity ? c->capacity*2 : 1024;
        while (capacity<c->size+n+1) capacity*=2;
        char *text=realloc(c->text,capacity);
        if (!text) return;
        c->text=text; c->capacity=capacity;
    }
    c->size+=(size_t)snprintf(c->text+c->size,c->capacity-c->size,"%c %s\n",kind,rel);
}
/* Whether the last close left `rel` with data (`closed`: the .closed text, "\n" first). */
static int closed_had_data(const char *closed, const char *rel) {
    char needle[800];
    snprintf(needle,sizeof(needle),"\nD %s\n",rel);
    return strstr(closed,needle)!=NULL;
}
/* backup=1: the files of `from` (the save) that hold data into `to` (its backup), each file's state into `out`.
 * backup=0: the files of `to` (the save) that are empty or all zeros put back from `from` (the backup), if `closed`
 * (the last close's states; NULL: not known) says that close left them with data. `rel`: the path in the save. */
static void backup_walk(const char *from, const char *to, int backup, int depth, const char *save, const char *rel,
                        Closed *out, const char *closed) {
    if (depth>4) return;
    DIR *dir=opendir(from);
    if (!dir) return;
    for (struct dirent *e; (e=readdir(dir));) {
        if (is_temp_name(e->d_name)) continue;
        char src[760], dst[760], path[700];
        snprintf(src,sizeof(src),"%s/%s",from,e->d_name);
        snprintf(dst,sizeof(dst),"%s/%s",to,e->d_name);
        snprintf(path,sizeof(path),"%s%s%s",rel,*rel ? "/" : "",e->d_name);
        struct stat st;
        if (stat(src,&st)) continue;
        if (S_ISDIR(st.st_mode)) {
            struct stat target;
            if (backup || (!stat(dst,&target) && S_ISDIR(target.st_mode)))
                backup_walk(src,dst,backup,depth+1,save,path,out,closed);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        size_t src_size, dst_size;
        unsigned char *src_data=read_whole(src,&src_size);
        if (!src_data) continue;
        struct stat target;
        const int dst_exists=!stat(dst,&target) && S_ISREG(target.st_mode);
        unsigned char *dst_data=dst_exists ? read_whole(dst,&dst_size) : NULL;
        if (backup) {
            if (out) closed_add(out,holds_data(src_data,src_size) ? 'D' : 'E',path);
            /* only a file with data, and only when it changed */
            if (holds_data(src_data,src_size) &&
                !(dst_data && dst_size==src_size && !memcmp(dst_data,src_data,src_size))) {
                if (make_dirs(to) || write_atomic(dst,src_data,src_size))
                    printf("Runtime: save backup of '%s/%s' could not be written\n",save,e->d_name);
            } else if (!holds_data(src_data,src_size)) {
                printf("Runtime: save '%s/%s' is %s: its backup is kept as it was\n",save,e->d_name,
                       src_size ? "all zeros" : "empty");
            }
        } else if (dst_exists && holds_data(src_data,src_size) && !holds_data(dst_data,dst_data ? dst_size : 0) &&
                   (dst_data || !target.st_size)) {
            /* (dst_data NULL with a size: unreadable, left alone) */
            if (closed && !closed_had_data(closed,path))
                printf("Runtime: save '%s/%s' is %s as the game left it at its last close: not restored\n",save,
                       e->d_name,target.st_size ? "all zeros" : "empty");
            else if (write_atomic(dst,src_data,src_size))
                printf("Runtime: save '%s/%s' is %s and could not be restored from its backup\n",save,e->d_name,
                       target.st_size ? "all zeros" : "empty");
            else
                printf("Runtime: save '%s/%s' was %s: restored from its backup (%zu bytes, backups/auto)\n",save,
                       e->d_name,target.st_size ? "all zeros" : "empty",src_size);
        }
        free(src_data);
        free(dst_data);
    }
    closedir(dir);
}
static void backup_path(const char *base, const char *name, char *out, size_t size) {
    snprintf(out,size,"%s.backups/auto/%s",base,name);
}
/* host: the save's folder; only the player's own saves (base/name) are backed up. */
static int backup_target(const char *host, char *base_out, size_t base_size, char *name_out, size_t name_size) {
    const char *slash=strrchr(host,'/');
    if (!slash || !backup_enabled()) return 0;
    snprintf(base_out,base_size,"%.*s",(int)(slash-host),host);
    snprintf(name_out,name_size,"%s",slash+1);
    return !strstr(base_out,".backups");
}
static void save_backup(const char *host) {
    char base[700], name[64], backup[800], closed_path[820];
    if (!backup_target(host,base,sizeof(base),name,sizeof(name))) return;
    backup_path(base,name,backup,sizeof(backup));
    snprintf(closed_path,sizeof(closed_path),"%s/.closed",backup);
    Closed closed={0};
    pthread_mutex_lock(&backup_lock);
    backup_walk(host,backup,1,0,name,"",&closed,NULL);
    if (make_dirs(backup) || write_atomic(closed_path,closed.text ? closed.text : "",closed.size))
        printf("Runtime: save backup state of '%s' could not be written\n",name);
    pthread_mutex_unlock(&backup_lock);
    free(closed.text);
}
static void save_repair(const char *host) {
    char base[700], name[64], backup[800], closed_path[820];
    if (!backup_target(host,base,sizeof(base),name,sizeof(name))) return;
    backup_path(base,name,backup,sizeof(backup));
    snprintf(closed_path,sizeof(closed_path),"%s/.closed",backup);
    pthread_mutex_lock(&backup_lock);
    size_t size=0;
    unsigned char *text=read_whole(closed_path,&size);
    char *closed=NULL;
    if (text) {
        closed=malloc(size+2);
        if (closed) { closed[0]='\n'; memcpy(closed+1,text,size); closed[size+1]=0; }
        free(text);
    }
    backup_walk(backup,host,0,0,name,"",NULL,closed);
    pthread_mutex_unlock(&backup_lock);
    free(closed);
}
/* The save's backup moved aside to <dir name><suffix> (replacing an older one there). */
static void save_backup_aside(const char *host, const char *suffix, const char *why) {
    char base[700], name[64], backup[800], old[820];
    if (!backup_target(host,base,sizeof(base),name,sizeof(name))) return;
    backup_path(base,name,backup,sizeof(backup));
    snprintf(old,sizeof(old),"%s%s",backup,suffix);
    pthread_mutex_lock(&backup_lock);
    struct stat st;
    if (!stat(backup,&st)) {
        remove_tree(old);
        if (rename(backup,old)) remove_tree(backup);
        if (why) printf("Runtime: save '%s' %s: its old backup moved to backups/auto/%s%s\n",name,why,name,suffix);
    }
    pthread_mutex_unlock(&backup_lock);
}

static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    initialized=0; return 0;
}
/* The overlay's save slots (gpu/shim/bbport_saves.cpp): the folder of this title's saves, whether the game has
 * any save open right now, and a slot chosen in the overlay ("Load", which restarts the game) put back before
 * the game opens its first save. */
static int32_t last_user=1;
void bbport_saves_apply_pending(void);
void bbport_saves_closed(const char *host_dir);
void bbport_saves_before_mount(const char *dir, int read_only);
void bbport_saves_mounted(const char *dir, int read_only);
/* A request from before the restart (a Load from the overlay's Saves page, Keep this progress), applied once before
 * the game first looks at its saves: its first search (what Continue starts) or its first mount. */
static pthread_once_t pending_once=PTHREAD_ONCE_INIT;
static void apply_pending_once(void) { pthread_once(&pending_once,bbport_saves_apply_pending); }
int runtime_savedata_dir(char *out, size_t size) { root(last_user,NULL,out,size); return 0; }
int runtime_savedata_mounted(void) {
    pthread_mutex_lock(&lock);
    int n=0;
    for (int i=0;i<SLOTS;++i) n+=slots[i].used;
    pthread_mutex_unlock(&lock);
    return n;
}
/* Practice mode (checkpoint jumps, runtime_checkpoint.c): each campaign save of the running game (SAVEDATA0<game>0
 * to 9) is the practice copy the overlay made before the first jump (BbSaves::StartPractice), for mounting and for
 * deleting alike (a game deleting its campaign save, on New Game say, must not delete the player's own). Not only
 * the first: a game playing on its second campaign save (0.3.6.52's Chapter Select made one; Continue picks the
 * newest) wrote the jumps into the player's save there. The profile saves (SAVEDATA0<game>P, CC) stay as they are. */
/* The running game (1-3) while practice mode is on, else 0. */
static int practice_game(void) {
    if (!runtime_checkpoint_practice()) return 0;
    const char *exe=getenv("BB_EXE");
    return !exe ? 0 : strstr(exe,"big3") ? 3 : strstr(exe,"big2") ? 2 : !strcmp(exe,"eboot.bin") ? 1 : 0;
}
/* SAVEDATA0<game>0 to 9: a campaign save of `game`. */
static int campaign_name(const char *name, int game) {
    char campaign[16];
    snprintf(campaign,sizeof(campaign),"SAVEDATA0%d",game);
    return game && strlen(name)==11 && !strncmp(name,campaign,10) && name[10]>='0' && name[10]<='9';
}
static void practice_redirect(const char *base, const char *name, char *host, size_t host_size, char *meta,
                              size_t meta_size) {
    const int game=practice_game();
    if (campaign_name(name,game)) {
        snprintf(host,host_size,"%s.backups/u%d/practice/%s",base,game,name);
        snprintf(meta,meta_size,"%s.backups/u%d/practice/%s.sce_sys",base,game,name);
    }
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    last_user=user;
    apply_pending_once();
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    bbport_saves_before_mount(dir->data,(mode & MODE_RDONLY)!=0);   /* an Undo picked in the Load list, put back now */
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    practice_redirect(base,dir->data,host,sizeof(host),meta,sizeof(meta));
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    if (exists) save_repair(host); /* an empty or zeroed file (a power cut while saving) from its backup */
    else save_backup_aside(host,".stale","has no folder"); /* new, or deleted outside the game */
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    slots[slot].writable=!(mode & MODE_RDONLY);
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    if (runtime_file_mount(result->point.data,host)) {
        /* No mount behind it: say so rather than report a mount every file call would fail on. */
        slots[slot].used=0;
        pthread_mutex_unlock(&lock);
        return ERR_MOUNT_FULL;
    }
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    /* Open now: a campaign save is the one the game plays on (a probe of a save that is not there changes nothing). */
    bbport_saves_mounted(dir->data,(mode & MODE_RDONLY)!=0);
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,m->title ? m->title->data : NULL,m->dir,m->mode,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,NULL,m->dir,m->mode,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    char host[700]="", picture[720]="";
    if (slot>=0) {
        runtime_file_unmount(point->data); slots[slot].used=0; snprintf(host,sizeof(host),"%s",slots[slot].host);
        /* (BilalReloaded, PR #349 by the-best-is-best) a save the game wrote gets the last picture of the game in
         * play as thumb.png beside its param, shown in the save list (bbport_thumbnail.cpp). */
        if (slots[slot].writable) snprintf(picture,sizeof(picture),"%s/thumb.png",slots[slot].meta);
    }
    pthread_mutex_unlock(&lock);
    /* No picture for it (none of its game's play yet, or not saved by hand yet): it keeps the one it has. */
    if (picture[0] && bbgpu_thumbnail_save(picture)) printf("Runtime: save picture written (%s)\n",picture);
    /* A save closed: its files with data are backed up (save auto-repair), and the overlay's story save
     * follows the game's campaign saves (bbport_saves.cpp). */
    if (slot>=0) save_backup(host);
    if (slot>=0) bbport_saves_closed(host);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        r=write_atomic(path,icon->buffer,icon->data_size) ? ERR_INTERNAL : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    practice_redirect(base,d->dir->data,host,sizeof(host),meta,sizeof(meta));
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    remove_tree(host);
    remove_tree(meta);
    save_backup_aside(host,".deleted",NULL);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* An existing slot of a save list (slots that do not exist are not listed, as on the console):
 * which = 0 first, 1 last, 2 latest written, 3 oldest, 4 the one named focus.
 * Returns 1 with the slot in out (and its parameters), or -1 when there is none. */
int runtime_savedata_pick(int32_t user, const char *title, const char (*names)[32], uint32_t count,
                          int which, const char *focus, char out[32], void *param_out) {
    char base[600];
    root(user,title,base,sizeof(base));
    int best=-1;
    Param p, best_param;
    memset(&best_param,0,sizeof(best_param));
    for (uint32_t i=0;names && i<count;++i) {
        char name[33]; memcpy(name,names[i],32); name[32]=0;
        if (!valid_name(name,sizeof(name))) continue;
        char host[640], meta[680];
        snprintf(host,sizeof(host),"%s/%s",base,name);
        snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,name);
        struct stat st;
        if (stat(host,&st) || !S_ISDIR(st.st_mode)) continue;
        read_param(meta,&p);
        int better = which==0 ? best<0 : which==1 ? 1 : which==2 ? best<0 || p.mtime>best_param.mtime
                   : which==3 ? best<0 || p.mtime<best_param.mtime : focus && !strncmp(focus,name,32);
        if (better) { best=(int)i; best_param=p; }
    }
    if (best<0) return -1;
    memcpy(out,names[best],32);
    if (param_out) memcpy(param_out,&best_param,sizeof(best_param));
    return 1;
}
/* ---- The save list (runtime_services.c, the overlay's list for sceSaveDataDialog), ported from BilalReloaded's
 * port (PR #349 by the-best-is-best; GPL-2.0-or-later): a save's host folders as the game would mount them (the
 * practice copy in practice mode), its picture, its label, and a delete that moves it to a trash folder. ---- */
static void list_paths(int32_t user, const char *title, const char *dir, char *base, size_t base_size, char host[640],
                       char meta[680]) {
    root(user,title,base,base_size);
    snprintf(host,640,"%s/%s",base,dir);
    snprintf(meta,680,"%s/%s.sce_sys",base,dir);
    practice_redirect(base,dir,host,640,meta,680);
}
int runtime_savedata_exists(int32_t user, const char *title, const char *dir) {
    if (!valid_name(dir,32)) return 0;
    char base[600], host[640], meta[680];
    list_paths(user,title,dir,base,sizeof(base),host,meta);
    struct stat st;
    return !stat(host,&st) && S_ISDIR(st.st_mode);
}
/* Host path of a save's picture: thumb.png (the game in play when it was written) when there is one, else the
 * game's icon0.png; -1 when neither. */
int runtime_savedata_icon(int32_t user, const char *title, const char *dir, char *out, size_t size) {
    if (!valid_name(dir,32)) return -1;
    char base[600], host[640], meta[680];
    list_paths(user,title,dir,base,sizeof(base),host,meta);
    struct stat st;
    snprintf(out,size,"%s/thumb.png",meta);
    if (!stat(out,&st) && st.st_size>0) return 0;
    snprintf(out,size,"%s/icon0.png",meta);
    return !stat(out,&st) && st.st_size>0 ? 0 : -1;
}
/* A param text field, at most `max` bytes, cut at a whole UTF-8 character (a cut inside one showed U+FFFD and
 * swallowed the line break after it). Arabic titles come in presentation forms (U+FE70..U+FEFF, already in display
 * order) with '_' for some spaces: those show as spaces. Trailing line breaks go. (the-best-is-best, PR #349) */
static void list_text(char *out, size_t size, const char *field, size_t max) {
    size_t n=strnlen(field,max);
    if (n>=size) n=size-1;
    if (n<strnlen(field,max+1)) {
        size_t start=n;
        while (start>0 && ((unsigned char)field[start-1]&0xc0)==0x80) --start;
        if (start>0) {
            const unsigned char lead=(unsigned char)field[start-1];
            const size_t need=lead>=0xf0 ? 4 : lead>=0xe0 ? 3 : lead>=0xc0 ? 2 : 1;
            if (n-(start-1)<need) n=start-1;
        }
    }
    memcpy(out,field,n);
    out[n]=0;
    int arabic=0;
    for (size_t i=0;i+1<n;++i)
        if ((unsigned char)out[i]==0xef && (unsigned char)out[i+1]>=0xb9 && (unsigned char)out[i+1]<=0xbb) { arabic=1; break; }
    if (arabic) for (size_t i=0;i<n;++i) if (out[i]=='_') out[i]=' ';
    while (n && (out[n-1]=='\n' || out[n-1]=='\r' || out[n-1]==' ')) out[--n]=0;
}
/* Title, subtitle (chapter and progress), detail (play time, treasures) and the date on lines of their own. */
void runtime_savedata_label(int32_t user, const char *title_in, const char *dir, char *out, size_t size) {
    char base[600], host[640], meta[680];
    Param p;
    if (!valid_name(dir,32)) { snprintf(out,size,"?"); return; }
    list_paths(user,title_in,dir,base,sizeof(base),host,meta);
    if (read_param(meta,&p)) { snprintf(out,size,"%s",dir); return; }
    time_t t=(time_t)p.mtime; struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char when[32]; strftime(when,sizeof(when),"%Y-%m-%d %H:%M",&tm);
    char title[128], subtitle[128], detail[320];
    list_text(title,sizeof(title),p.title,127);
    list_text(subtitle,sizeof(subtitle),p.subtitle,127);
    list_text(detail,sizeof(detail),p.detail,1023);
    snprintf(out,size,"%s\n%s%s%s%s%s",title[0] ? title : dir,subtitle,subtitle[0] ? "\n" : "",detail,
             detail[0] ? "\n" : "",when);
}
/* Deleting a save from the save list: its folders (and its auto backup) move to
 * <user dir>/savedata-deleted/<user>/<title>/<dir>-<time>, never removed, so a mistake can be undone by moving them
 * back. 0 when moved; an open (mounted) save is refused. */
int runtime_savedata_trash(int32_t user, const char *title, const char *dir) {
    if (!valid_name(dir,32)) return -1;
    char base[600], host[640], meta[680], backup[800], trash[760], to[840];
    list_paths(user,title,dir,base,sizeof(base),host,meta);
    {
        const char *slash=strrchr(host,'/');
        char hbase[700]; snprintf(hbase,sizeof(hbase),"%.*s",slash ? (int)(slash-host) : 0,host);
        backup_path(hbase,dir,backup,sizeof(backup));
    }
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i)
        if (slots[i].used && !strcmp(slots[i].host,host)) {
            pthread_mutex_unlock(&lock);
            printf("Runtime: save data '%s' is open: not deleted\n",dir);
            return -1;
        }
    char stamp[32]; time_t now=time(NULL); struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&now);
#else
    localtime_r(&now,&tm);
#endif
    strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",&tm);
    snprintf(trash,sizeof(trash),"%s/savedata-deleted/%d/%s/%s-%s",runtime_file_user_dir(),user,
             title && *title ? title : title_id,dir,stamp);
    struct stat st;
    int r=stat(host,&st) || make_dirs(trash) ? -1 : 0;
    if (!r) { snprintf(to,sizeof(to),"%s/%s",trash,dir); r=rename(host,to) ? -1 : 0; }
    if (!r && !stat(meta,&st)) { snprintf(to,sizeof(to),"%s/%s.sce_sys",trash,dir); rename(meta,to); }
    pthread_mutex_unlock(&lock);
    if (!r) {
        pthread_mutex_lock(&backup_lock);
        if (!stat(backup,&st)) { snprintf(to,sizeof(to),"%s/backup",trash); rename(backup,to); }
        pthread_mutex_unlock(&backup_lock);
    }
    printf("Runtime: save data '%s' %s %s\n",dir,r ? "could not be moved to" : "moved to",trash);
    return r;
}
/* The overlay's Saves page (bbport_saves.cpp): a save of the running game deleted as from the save list. */
int runtime_savedata_trash_save(const char *dir) { return runtime_savedata_trash(last_user,NULL,dir); }
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    if (cond->user>=0) last_user=cond->user;
    apply_pending_once();
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    /* Practice mode: the campaign saves are the practice copies, as for mounting and deleting (practice_redirect):
     * the player's own are left out and the practice folder's listed, so a campaign save the game creates while
     * practising (Chapter Select or New Game onto New save) is found right after, and one only in the player's
     * folder is not offered for a mount that would not find it. */
    const int game=practice_game();
    char practice[640];
    snprintf(practice,sizeof(practice),"%s.backups/u%d/practice",base,game);
    for (int pass=0;pass<(game ? 2 : 1);++pass) {
        const char *folder=pass ? practice : base;
        DIR *d=opendir(folder);
        for (struct dirent *e; d && (e=readdir(d));) {
            size_t n=strlen(e->d_name);
            if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys"))) continue;
            if (game && campaign_name(e->d_name,game)!=pass) continue;
            if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
            if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); }
            snprintf(entries[count].name,32,"%s",e->d_name);
            char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",folder,e->d_name);
            read_param(meta,&entries[count].param);
            ++count;
        }
        if (d) closedir(d);
    }
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title. */
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size) return ERR_PARAMETER;
    char base[600], dir[640];
    root(user,NULL,base,sizeof(base));
    snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    snprintf(memory_path,sizeof(memory_path),"%s/memory.dat",dir);
    FILE *f=fopen(memory_path,"r+b");
    if (!f) f=fopen(memory_path,"w+b");
    int32_t r=ERR_INTERNAL;
    if (f) {
        fseek(f,0,SEEK_END);
        if ((uint64_t)ftell(f)<size && ftruncate(fileno(f),(off_t)size)) goto done;
        memory_size=size; r=0;
    done:
        fclose(f);
    }
    if (!r && param) write_param(dir,param);
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory ready (%llu bytes)\n",(unsigned long long)size);
    return r;
}
static int32_t memory_io(void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memory_size) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memory_size) { pthread_mutex_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memory_path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,1); }

static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",save_umount},
    {"sceSaveDataSetParam",save_set_param}, {"sceSaveDataSaveIcon",save_icon}, {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }

