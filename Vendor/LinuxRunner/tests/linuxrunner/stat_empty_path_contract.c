#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_newfstatat
#ifdef __aarch64__
#define SYS_newfstatat 79
#else
#define SYS_newfstatat SYS_fstatat
#endif
#endif
#ifndef SYS_statx
#ifdef __aarch64__
#define SYS_statx 291
#endif
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef AT_NO_AUTOMOUNT
#define AT_NO_AUTOMOUNT 0x800
#endif
#define SYNC_FORCE 0x2000
#define SYNC_DONT 0x4000
#define BASIC_STATS 0x7ffU

// Independent fixed-width Linux UAPI layout, including musl fixtures that do
// not expose struct statx. Both syscalls are exercised directly, not through
// libc's statx-to-fstatat fallback.
struct guest_statx_timestamp { int64_t sec; uint32_t nsec; int32_t reserved; };
struct guest_statx {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad;
    uint64_t ino, size, blocks, attributes_mask;
    struct guest_statx_timestamp atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t spare[14];
};
_Static_assert(sizeof(struct guest_statx) == 256, "statx UAPI size");
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while (0)
#define ERROR(call, expected) do { errno=0; CHECK((call)==-1 && errno==(expected)); } while (0)
static long at_stat(int fd, const char *path, struct stat *st, int flags) {
    return syscall(SYS_newfstatat, fd, path, st, flags);
}
static long at_statx(int fd, const char *path, int flags, unsigned mask, struct guest_statx *st) {
    return syscall(SYS_statx, fd, path, flags, mask, st);
}
int main(void) {
    char directory[]="/tmp/lr-stat-XXXXXX", path[256], renamed[256], linkpath[256];
    struct stat file, st, cwd_stat;
    struct guest_statx sx;
    CHECK(mkdtemp(directory));
    snprintf(path,sizeof(path),"%s/source",directory);
    snprintf(renamed,sizeof(renamed),"%s/renamed",directory);
    snprintf(linkpath,sizeof(linkpath),"%s/link",directory);
    int cwd=open(".",O_RDONLY|O_DIRECTORY), dir=open(directory,O_RDONLY|O_DIRECTORY);
    CHECK(cwd>=0 && dir>=0);
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);
    CHECK(fd>=0 && write(fd,"payload",7)==7 && fstat(fd,&file)==0);
    CHECK(at_stat(fd,"",&st,AT_EMPTY_PATH)==0 && st.st_ino==file.st_ino && st.st_size==7);
    CHECK(at_statx(fd,"",AT_EMPTY_PATH,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino && sx.size==7 && (sx.mask&BASIC_STATS)==BASIC_STATS);
    CHECK(at_stat(fd,NULL,&st,AT_EMPTY_PATH)==0 && st.st_ino==file.st_ino);
    CHECK(at_statx(fd,NULL,AT_EMPTY_PATH,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino);
    int opath=open(path,O_PATH|O_CLOEXEC); CHECK(opath>=0);
    CHECK(at_stat(opath,"",&st,AT_EMPTY_PATH)==0 && st.st_ino==file.st_ino);
    CHECK(at_statx(opath,"",AT_EMPTY_PATH,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino);
    CHECK(at_stat(-1,path,&st,0)==0 && st.st_ino==file.st_ino);
    CHECK(at_statx(-1,path,0,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino);
    CHECK(at_stat(dir,"source",&st,0)==0 && st.st_ino==file.st_ino);
    CHECK(at_statx(dir,"source",0,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino);
    CHECK(symlink("source",linkpath)==0);
    CHECK(at_stat(dir,"link",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode));
    CHECK(at_statx(dir,"link",AT_SYMLINK_NOFOLLOW,BASIC_STATS,&sx)==0 && S_ISLNK(sx.mode));
    CHECK(at_stat(dir,"link",&st,0)==0 && st.st_ino==file.st_ino);
    CHECK(at_statx(dir,"link",AT_NO_AUTOMOUNT|SYNC_FORCE,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino);
    CHECK(at_statx(dir,"link",SYNC_DONT,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino);
    ERROR(at_stat(fd,"child",&st,0),ENOTDIR);
    ERROR(at_statx(fd,"child",0,BASIC_STATS,&sx),ENOTDIR);
    ERROR(at_stat(-1,"relative",&st,0),EBADF);
    ERROR(at_statx(-1,"relative",0,BASIC_STATS,&sx),EBADF);
    ERROR(at_stat(fd,"",&st,0),ENOENT);
    ERROR(at_statx(fd,"",0,BASIC_STATS,&sx),ENOENT);
    ERROR(at_stat(-1,"",&st,AT_EMPTY_PATH),EBADF);
    ERROR(at_statx(-1,"",AT_EMPTY_PATH,BASIC_STATS,&sx),EBADF);
    ERROR(at_stat(fd,"",&st,0x40000000),EINVAL);
    ERROR(at_statx(fd,"",AT_EMPTY_PATH|0x40000000,BASIC_STATS,&sx),EINVAL);
    ERROR(at_statx(fd,"",AT_EMPTY_PATH|SYNC_FORCE|SYNC_DONT,BASIC_STATS,&sx),EINVAL);
    ERROR(at_statx(fd,"",AT_EMPTY_PATH,0x80000000U,&sx),EINVAL);
    ERROR(at_stat(fd,NULL,&st,0),EFAULT);
    ERROR(at_statx(fd,NULL,0,BASIC_STATS,&sx),EFAULT);
    ERROR(at_stat(fd,"",NULL,AT_EMPTY_PATH),EFAULT);
    ERROR(at_statx(fd,"",AT_EMPTY_PATH,BASIC_STATS,NULL),EFAULT);
    CHECK(fchdir(dir)==0 && fstat(dir,&cwd_stat)==0);
    CHECK(at_stat(AT_FDCWD,"",&st,AT_EMPTY_PATH)==0 && st.st_ino==cwd_stat.st_ino && S_ISDIR(st.st_mode));
    CHECK(at_statx(AT_FDCWD,"",AT_EMPTY_PATH,BASIC_STATS,&sx)==0 && sx.ino==cwd_stat.st_ino && S_ISDIR(sx.mode));
    CHECK(at_stat(AT_FDCWD,NULL,&st,AT_EMPTY_PATH)==0 && st.st_ino==cwd_stat.st_ino);
    CHECK(at_statx(AT_FDCWD,NULL,AT_EMPTY_PATH,BASIC_STATS,&sx)==0 && sx.ino==cwd_stat.st_ino);
    CHECK(fchdir(cwd)==0);
    CHECK(rename(path,renamed)==0 && unlink(renamed)==0);
    CHECK(at_stat(fd,"",&st,AT_EMPTY_PATH)==0 && st.st_ino==file.st_ino && st.st_size==7 && st.st_nlink==0);
    CHECK(at_statx(opath,"",AT_EMPTY_PATH,BASIC_STATS,&sx)==0 && sx.ino==file.st_ino && sx.size==7 && sx.nlink==0);
    CHECK(unlink(linkpath)==0);
    close(opath); close(fd); close(dir); close(cwd);
    CHECK(rmdir(directory)==0);
    printf("STAT_EMPTY_PATH_CONTRACT_PASS checks=%u fd=PASS cwd=PASS null=PASS absolute=PASS relative=PASS symlink=PASS rename_unlink=PASS flags=PASS\n",checks);
    return 0;
}
