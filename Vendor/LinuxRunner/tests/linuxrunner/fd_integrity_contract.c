#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(name, expression) do { int good = !!(expression); ++checks; if (!good) ++failures; printf("FD_CHECK %s %s errno=%d\n", name, good ? "PASS" : "FAIL", errno); fflush(stdout); } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int descriptor_count(void) { int count=0; for(int i=0;i<256;i++) if(fcntl(i,F_GETFD)>=0)count++; return count; }
static void *duplicate_worker(void *arg) { int source=*(int*)arg; for(int i=0;i<1000;i++){ int d=fcntl(source,F_DUPFD_CLOEXEC,64); if(d<0)return (void*)1; if(fcntl(d,F_GETFD)!=FD_CLOEXEC)return (void*)1; close(d); } return NULL; }
static volatile sig_atomic_t alarm_seen;
static void alarm_handler(int sig) { (void)sig; alarm_seen=1; }
static volatile sig_atomic_t trap_seen;
static void trap_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
#if defined(__aarch64__)
    ucontext_t *uc=ctx;
    if(info->si_signo==SIGTRAP && info->si_code==TRAP_BRKPT && (uintptr_t)info->si_addr==uc->uc_mcontext.pc && uc->uc_mcontext.regs[19]==0x12345678) trap_seen=1;
    uc->uc_mcontext.pc += 4;
#else
    (void)info;(void)ctx;
#endif
}
int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"child")) {
        int fd=atoi(argv[2]), closed=atoi(argv[3]); char c=0;
        return read(fd,&c,1)==1 && c=='I' && fcntl(closed,F_GETFD)<0 && errno==EBADF ? 0:9;
    }
    if(argc==2 && !strcmp(argv[1],"host-probe")) {
        puts("HOST_FD_BEFORE"); fflush(stdout); sleep(2);
        for(int i=0;i<2000;i++) {
            syscall(SYS_pipe2,(void *)1,0);
            int ends[2];if(pipe(ends))return 2;
            struct pollfd x={ends[0],POLLIN,0}; if(poll(&x,1,0))return 3;
            close(ends[0]);close(ends[1]);
        }
        puts("HOST_FD_AFTER"); fflush(stdout); sleep(2);return 0;
    }
    int base=descriptor_count(),p[2];
    CHECK("pipe",pipe2(p,O_NONBLOCK|O_CLOEXEC)==0);
    if(failures)return 1;
    CHECK("pipe_access",(fcntl(p[0],F_GETFL)&O_ACCMODE)==O_RDONLY && (fcntl(p[1],F_GETFL)&O_ACCMODE)==O_WRONLY);
    CHECK("dup2_same",dup2(p[0],p[0])==p[0]);
    CHECK("dup3_same",dup3(p[0],p[0],0)<0 && errno==EINVAL);
    CHECK("dup3_flags",dup3(p[0],40,123)<0 && errno==EINVAL);
    CHECK("dup3_negative",dup3(p[0],-1,0)<0 && errno==EBADF);
    CHECK("fcntl_negative",fcntl(p[0],F_DUPFD,-1)<0 && errno==EINVAL);
    CHECK("dup3_cloexec",dup3(p[0],40,O_CLOEXEC)==40 && fcntl(40,F_GETFD)==FD_CLOEXEC);
    CHECK("dup3_clear_cloexec",dup3(p[0],40,0)==40 && fcntl(40,F_GETFD)==0);close(40);
    int d=dup(p[0]);CHECK("dup_clear_cloexec",d>=0 && fcntl(d,F_GETFD)==0);
    CHECK("dup_shared_flags",fcntl(d,F_SETFL,0)==0 && !(fcntl(p[0],F_GETFL)&O_NONBLOCK));
    close(d);fcntl(p[0],F_SETFL,O_NONBLOCK);
    char path[80],link[128];snprintf(path,sizeof path,"/dev/fd/%d",p[0]);
    int opened=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    CHECK("dev_fd_pipe_open",opened>=0);
    if(opened>=0){
        CHECK("reopen_independent_flags",fcntl(opened,F_SETFL,0)==0 && (fcntl(p[0],F_GETFL)&O_NONBLOCK));
        struct stat st;CHECK("magic_pipe_stat",stat(path,&st)==0 && S_ISFIFO(st.st_mode));
        ssize_t n=readlink(path,link,sizeof(link)-1);if(n>=0)link[n]=0;
        CHECK("pipe_link_identity",n>0 && !strncmp(link,"pipe:[",6));
        CHECK("pipe_transfer",write(p[1],"P",1)==1 && read(opened,link,1)==1 && link[0]=='P');close(opened);
    }
    snprintf(path,sizeof path,"/proc/self/fd/%d",p[1]);opened=open(path,O_WRONLY|O_NONBLOCK);
    CHECK("proc_fd_write_open",opened>=0);if(opened>=0)close(opened);
    CHECK("nofollow_magic",open(path,O_WRONLY|O_NOFOLLOW)<0 && errno==ELOOP);
    char search_dir[]="/tmp/lr-search.XXXXXX";CHECK("search_dir_create",mkdtemp(search_dir)!=NULL && chmod(search_dir,0711)==0);
    int directory=open(search_dir,O_RDONLY|O_DIRECTORY);
    pid_t search_child=fork();if(!search_child){setgroups(0,NULL);if(setresgid(23456,23456,23456)||setresuid(23456,23456,23456))_exit(4);
        int good=chdir(search_dir)==0 && fchdir(directory)==0 && open(search_dir,O_RDONLY|O_DIRECTORY)<0 && errno==EACCES;
        _exit(good?0:7);}
    int search_status=0;waitpid(search_child,&search_status,0);CHECK("search_only_chdir",WIFEXITED(search_status)&&WEXITSTATUS(search_status)==0);
    CHECK("fchdir_regular_type",fchdir(p[0])<0 && errno==ENOTDIR);close(directory);rmdir(search_dir);
    CHECK("pread_pipe_espipe",pread(p[0],link,1,0)<0 && errno==ESPIPE);
    CHECK("pwrite_pipe_espipe",pwrite(p[1],link,1,0)<0 && errno==ESPIPE);
    struct pollfd polls[3]={{p[0],POLLIN,0},{p[0],POLLIN,0},{-1,POLLIN,0}};
    write(p[1],"D",1);CHECK("poll_duplicate_count",poll(polls,3,0)==2 && polls[0].revents==POLLIN && polls[1].revents==POLLIN && !polls[2].revents);
    read(p[0],link,1);
    struct pollfd invalid={100000,POLLIN,0};double t=now();int r=poll(&invalid,1,700);
    CHECK("poll_invalid_immediate",r==1 && invalid.revents==POLLNVAL && now()-t<.3);
    struct timespec bad={0,1000000000};CHECK("ppoll_invalid_timeout",ppoll(NULL,0,&bad,NULL)<0 && errno==EINVAL);
    struct timespec tiny={0,100000};CHECK("ppoll_submillisecond",ppoll(NULL,0,&tiny,NULL)==0);
    CHECK("select_negative",select(-1,NULL,NULL,NULL,NULL)<0 && errno==EINVAL);
    fd_set rd,wr;FD_ZERO(&rd);FD_ZERO(&wr);FD_SET(p[0],&rd);FD_SET(p[1],&wr);write(p[1],"S",1);struct timeval zero={0};
    CHECK("select_requested_sets",select(p[1]+1,&rd,&wr,NULL,&zero)==2 && FD_ISSET(p[0],&rd) && FD_ISSET(p[1],&wr));read(p[0],link,1);
    struct rlimit saved;CHECK("prlimit_explicit_pid",prlimit(getpid(),RLIMIT_NOFILE,NULL,&saved)==0);
    struct rlimit badlim={saved.rlim_max+1,saved.rlim_max};CHECK("limit_cur_gt_max",setrlimit(RLIMIT_NOFILE,&badlim)<0 && errno==EINVAL);
    struct rlimit low={64,saved.rlim_max};CHECK("limit_lower",setrlimit(RLIMIT_NOFILE,&low)==0);
    CHECK("dup_out_of_range",dup3(p[0],64,0)<0 && errno==EBADF);
    CHECK("fcntl_out_of_range",fcntl(p[0],F_DUPFD,64)<0 && errno==EINVAL);
    struct pollfd too_many[65]={{-1,0,0}};CHECK("poll_nfds_limit",poll(too_many,65,0)<0 && errno==EINVAL);
    int slots[64],count=0;while(count<64 && (d=dup(p[0]))>=0)slots[count++]=d;
    CHECK("descriptor_emfile",d<0 && errno==EMFILE);
    CHECK("fcntl_cloexec_emfile",fcntl(p[0],F_DUPFD_CLOEXEC,0)<0 && errno==EMFILE);
    for(int i=0;i<count;i++) { close(slots[i]); }
    CHECK("limit_restore",setrlimit(RLIMIT_NOFILE,&saved)==0);
    int limit_pipe[2];CHECK("limit_child_pipe",pipe(limit_pipe)==0);
    pid_t limit_child=fork();if(!limit_child){close(limit_pipe[1]);char token;struct rlimit own;int ok=read(limit_pipe[0],&token,1)==1 && getrlimit(RLIMIT_NOFILE,&own)==0 && own.rlim_cur==32;_exit(ok?0:8);}
    close(limit_pipe[0]);struct rlimit child_old, child_low={32,saved.rlim_max}, child_query, parent_query;
    CHECK("prlimit_other_pid",prlimit(limit_child,RLIMIT_NOFILE,NULL,&child_old)==0 && child_old.rlim_cur==saved.rlim_cur);
    CHECK("prlimit_child_update",prlimit(limit_child,RLIMIT_NOFILE,&child_low,NULL)==0 && prlimit(limit_child,RLIMIT_NOFILE,NULL,&child_query)==0 && child_query.rlim_cur==32);
    CHECK("prlimit_group_isolation",getrlimit(RLIMIT_NOFILE,&parent_query)==0 && parent_query.rlim_cur==saved.rlim_cur);
    write(limit_pipe[1],"L",1);close(limit_pipe[1]);int limit_status=0;waitpid(limit_child,&limit_status,0);
    CHECK("prlimit_child_observes",WIFEXITED(limit_status)&&WEXITSTATUS(limit_status)==0);
    int inherited=fcntl(p[0],F_DUPFD,64),exec_closed=fcntl(p[0],F_DUPFD_CLOEXEC,65);
    write(p[1],"I",1);pid_t child=fork();if(child==0){char a[20],b[20];snprintf(a,sizeof a,"%d",inherited);snprintf(b,sizeof b,"%d",exec_closed);execl(argv[0],argv[0],"child",a,b,NULL);_exit(8);}int ws=0;waitpid(child,&ws,0);
    CHECK("fork_exec_cloexec",WIFEXITED(ws)&&WEXITSTATUS(ws)==0);close(inherited);close(exec_closed);
    pthread_t workers[4];int started=0;for(int i=0;i<4;i++)if(!pthread_create(&workers[i],NULL,duplicate_worker,&p[0]))started++;
    int worker_fail=started!=4;for(int i=0;i<started;i++){void *v=NULL;pthread_join(workers[i],&v);worker_fail|=v!=NULL;}CHECK("concurrent_dup_close",!worker_fail);
    for(int i=0;i<250;i++){int q[2];if(pipe2(q,O_CLOEXEC)<0){failures++;break;}struct pollfd x={q[0],POLLIN,0};if(poll(&x,1,0)!=0)failures++;close(q[0]);close(q[1]);}
    // EFAULT after host pipe allocation must release both endpoints exactly once.
    for(int i=0;i<100;i++)if(syscall(SYS_pipe2,(void*)1,0)!=-1||errno!=EFAULT)failures++;
    int q[2];CHECK("pipe_after_faults",pipe(q)==0);close(q[0]);close(q[1]);
    int sockets[2];CHECK("unix_socketpair",socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    char payload='F', ancillary[CMSG_SPACE(sizeof(int))]={0};
    struct iovec io={&payload,1}; struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=ancillary,.msg_controllen=sizeof ancillary};
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&p[0],sizeof(int));
    CHECK("unix_send_rights",sendmsg(sockets[0],&message,0)==1);
    memset(ancillary,0,sizeof ancillary);message.msg_controllen=sizeof ancillary;
    CHECK("unix_receive_rights",recvmsg(sockets[1],&message,MSG_CMSG_CLOEXEC)==1 && CMSG_FIRSTHDR(&message)!=NULL);
    int received=-1;header=CMSG_FIRSTHDR(&message);if(header)memcpy(&received,CMSG_DATA(header),sizeof(int));
    CHECK("unix_rights_cloexec",received>=0 && fcntl(received,F_GETFD)==FD_CLOEXEC);
    if(received>=0){write(p[1],"F",1);CHECK("unix_rights_data",read(received,&payload,1)==1 && payload=='F');close(received);}
    close(sockets[0]);close(sockets[1]);
    child=fork();if(!child){close(p[1]);setgroups(0,NULL);if(setresgid(23456,23456,23456)||setresuid(23456,23456,23456))_exit(5);
        snprintf(path,sizeof path,"/proc/self/fd/%d",p[0]);int fd=open(path,O_RDONLY|O_NONBLOCK);int denied=fd<0&&errno==EACCES;struct rlimit parent_limit;denied=denied && prlimit(getppid(),RLIMIT_NOFILE,NULL,&parent_limit)<0 && errno==EPERM;
        snprintf(path,sizeof path,"/proc/%d/fd/%d",getppid(),p[0]);ssize_t n=readlink(path,link,sizeof link);_exit(denied&&n<0&&errno==EACCES?0:6);}
    waitpid(child,&ws,0);CHECK("pipe_inode_dac_after_drop",WIFEXITED(ws)&&WEXITSTATUS(ws)==0);
    int blocked[2];CHECK("blocking_pipe_create",pipe(blocked)==0);
    struct sigaction interrupted={.sa_handler=alarm_handler};sigemptyset(&interrupted.sa_mask);sigaction(SIGALRM,&interrupted,NULL);
    alarm(1);int blocked_result=read(blocked[0],&payload,1);alarm(0);
    CHECK("blocking_pipe_signal",blocked_result<0 && errno==EINTR && alarm_seen);
    close(blocked[0]);close(blocked[1]);
    close(p[0]);close(p[1]);
    CHECK("descriptor_cleanup",descriptor_count()==base);
    // SIGTRAP is delivered, registers survive, and only a guest handler advances PC.
    struct sigaction sa={.sa_sigaction=trap_handler,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);sigaction(SIGTRAP,&sa,NULL);
#if defined(__aarch64__)
    asm volatile("mov x19, #0x5678\nmovk x19, #0x1234, lsl #16\nbrk #0" ::: "x19","memory");
    CHECK("brk_signal_registers",trap_seen);
#endif
    printf("FD_CONTRACT checks=%d failures=%d remaining_descriptors=%d baseline=%d\n",checks,failures,descriptor_count(),base);
    return failures?1:0;
}
