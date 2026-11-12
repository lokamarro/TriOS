#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
// Actual guest filesystem, including child interruption after durable publish.
int main(void) {
    char path[]="/tmp/lr-pacman-fs.XXXXXX", data[8]={0};
    assert(mkdtemp(path)); int dir=open(path,O_RDONLY|O_DIRECTORY); assert(dir>=0);
    assert(mkdirat(dir,"package",0755)==0);
    int pkg=openat(dir,"package",O_RDONLY|O_DIRECTORY); assert(pkg>=0);
    int fd=openat(pkg,"desc.new",O_CREAT|O_EXCL|O_RDWR,0644); assert(fd>=0);
    assert(write(fd,"old-data",8)==8); assert(pwrite(fd,"new",3,0)==3);
    assert(fchmod(fd,0644)==0); assert(fchown(fd,0,0)==0);
    assert(fdatasync(fd)==0); assert(close(fd)==0);
    assert(renameat(pkg,"desc.new",pkg,"desc")==0); assert(fsync(pkg)==0);
    int channel[2]; assert(pipe(channel)==0); pid_t child=fork(); assert(child>=0);
    if(!child) {
        close(channel[0]); int x=openat(pkg,"files.new",O_CREAT|O_EXCL|O_WRONLY,0644);
        assert(x>=0); assert(write(x,"payload\n",8)==8); assert(fsync(x)==0); assert(close(x)==0);
        assert(renameat(pkg,"files.new",pkg,"files")==0); assert(fsync(pkg)==0);
        assert(write(channel[1],"y",1)==1); for(;;) pause();
    }
    close(channel[1]); assert(read(channel[0],data,1)==1); assert(kill(child,SIGKILL)==0);
    int status; assert(waitpid(child,&status,0)==child && WIFSIGNALED(status));
    fd=openat(pkg,"desc",O_RDONLY); assert(fd>=0 && read(fd,data,8)==8 && !memcmp(data,"new-data",8)); close(fd);
    fd=openat(pkg,"files",O_RDONLY); assert(fd>=0 && read(fd,data,8)==8 && !memcmp(data,"payload\n",8)); close(fd);
    assert(renameat(dir,"package",dir,"package-final")==0); assert(fsync(dir)==0);
    assert(unlinkat(pkg,"desc",0)==0 && unlinkat(pkg,"files",0)==0); close(pkg);
    assert(unlinkat(dir,"package-final",AT_REMOVEDIR)==0); close(dir); assert(rmdir(path)==0);
    puts("PACKAGE_FS_CONTRACT PASS"); return 0;
}
