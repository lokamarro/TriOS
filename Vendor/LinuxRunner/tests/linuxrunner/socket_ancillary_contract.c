#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks, failures;
static int quiet;
#define CHECK(name, expr) do { int ok = !!(expr); checks++; failures += !ok; \
 if(!quiet || !ok)printf("SCM_CHECK %s %s errno=%d\n", name, ok ? "PASS" : "FAIL", errno); } while (0)
static int file_with(char ch) {
 char name[] = "/tmp/lr-scm-XXXXXX";
 int fd = mkstemp(name); unlink(name);
 if (fd < 0 || write(fd, &ch, 1) != 1 || lseek(fd, 0, SEEK_SET) != 0) exit(90);
 return fd;
}
static int pair(int sv[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, sv); }
static ssize_t send_fds(int s, const int *fds, unsigned count, char byte) {
 char control[CMSG_SPACE(253*sizeof(int))] = {0};
 struct iovec io = {&byte, 1};
 struct msghdr m = {.msg_iov=&io, .msg_iovlen=1, .msg_control=control,
                    .msg_controllen=CMSG_SPACE(count*sizeof(int))};
 struct cmsghdr *c = CMSG_FIRSTHDR(&m);
 c->cmsg_len = CMSG_LEN(count*sizeof(int)); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS;
 memcpy(CMSG_DATA(c), fds, count*sizeof(int));
 return sendmsg(s, &m, 0);
}
static unsigned received_fds(struct msghdr *m, int out[253]) {
 unsigned count = 0;
 for (struct cmsghdr *c=CMSG_FIRSTHDR(m); c; c=CMSG_NXTHDR(m,c)) {
  if (c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
   unsigned n=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
   if(n>253-count) exit(91);
   memcpy(out+count,CMSG_DATA(c),n*sizeof(int)); count+=n;
  }
 }
 return count;
}
static void limit_case(unsigned slots) {
 int sv[2], src=file_with('L'); CHECK("limit_pair",pair(sv)==0);
 int rights[2]={src,src};CHECK("limit_send",send_fds(sv[0],rights,2,'L')==1);
 struct rlimit saved, small;CHECK("limit_get",getrlimit(RLIMIT_NOFILE,&saved)==0);
 small=saved;small.rlim_cur=32;CHECK("limit_set",setrlimit(RLIMIT_NOFILE,&small)==0);
 int fillers[64],n=0;
 while(n<64 && (fillers[n]=open("/dev/null",O_RDONLY))>=0)n++;
 CHECK("limit_exhausted",errno==EMFILE && n>=(int)slots);
 for(unsigned i=0;i<slots;i++)close(fillers[--n]);
 char byte=0,control[CMSG_SPACE(2*sizeof(int))]={0};int installed[253];
 struct iovec io={&byte,1};struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
 ssize_t r=recvmsg(sv[1],&m,MSG_CMSG_CLOEXEC);
 CHECK("limit_data_not_emfile",r==1 && byte=='L');
 if(r>=0){unsigned count=received_fds(&m,installed);CHECK("limit_partial_rights",count==slots);CHECK("limit_ctrunc",m.msg_flags&MSG_CTRUNC);for(unsigned i=0;i<count;i++){CHECK("limit_cloexec",fcntl(installed[i],F_GETFD)&FD_CLOEXEC);close(installed[i]);}}
 CHECK("limit_restore",setrlimit(RLIMIT_NOFILE,&saved)==0);
 while(n)close(fillers[--n]);close(src);close(sv[0]);close(sv[1]);
}
static void fault_then_receive(void) {
 int sv[2],a=file_with('A'),b=file_with('B');CHECK("fault_pair",pair(sv)==0);
 CHECK("fault_send_a",send_fds(sv[0],&a,1,'A')==1);close(a);
 long page=sysconf(_SC_PAGESIZE);void *bad=mmap(NULL,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(bad==MAP_FAILED)exit(92);
 char control[CMSG_SPACE(sizeof(int))]={0};struct iovec io={bad,1};
 struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
 errno=0;CHECK("fault_payload_efault",recvmsg(sv[1],&m,0)==-1 && errno==EFAULT);
 CHECK("fault_send_b",send_fds(sv[0],&b,1,'B')==1);close(b);
 char byte=0;io.iov_base=&byte;m.msg_controllen=sizeof control;memset(control,0,sizeof control);
 ssize_t r=recvmsg(sv[1],&m,0);CHECK("fault_next_payload",r==1&&(byte=='A'||byte=='B'));
 int installed[253];unsigned count=r>=0?received_fds(&m,installed):0;CHECK("fault_next_rights",count==1);
 if(count){char content=0;CHECK("fault_next_description",read(installed[0],&content,1)==1&&content==byte);}
 for(unsigned i=0;i<count;i++)close(installed[i]);
 // Linux retains the faulted stream payload; current native-import runtime
 // consumes it. Record that separate known gap, never label it conformance.
 if(!quiet)printf("SCM_FAULT_RETENTION %s\n",byte=='A'?"LINUX_MATCH":"KNOWN_NATIVE_IMPORT_LIMITATION");
 if(byte=='A') {m.msg_controllen=sizeof control;memset(control,0,sizeof control);CHECK("fault_remaining_b",recvmsg(sv[1],&m,0)==1&&byte=='B');count=received_fds(&m,installed);CHECK("fault_remaining_rights",count==1);for(unsigned i=0;i<count;i++)close(installed[i]);}
 munmap(bad,page);close(sv[0]);close(sv[1]);
}
static void zero_send(void) {
 int sv[2],a=file_with('A'),b=file_with('B');CHECK("zero_pair",pair(sv)==0);
 char control[CMSG_SPACE(sizeof(int))]={0},byte=0;struct iovec io={&byte,0};
 struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
 struct cmsghdr *c=CMSG_FIRSTHDR(&m);c->cmsg_len=CMSG_LEN(sizeof(int));c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;memcpy(CMSG_DATA(c),&a,sizeof a);
 CHECK("zero_byte_send",sendmsg(sv[0],&m,0)==0);close(a);
 CHECK("zero_next_send",send_fds(sv[0],&b,1,'B')==1);close(b);
 io.iov_len=1;memset(control,0,sizeof control);CHECK("zero_next_payload",recvmsg(sv[1],&m,0)==1&&byte=='B');
 int installed[253];unsigned count=received_fds(&m,installed);CHECK("zero_next_rights",count==1);CHECK("one_right_control_space",m.msg_controllen==CMSG_SPACE(sizeof(int)));
 if(count){char value=0;CHECK("zero_next_description",read(installed[0],&value,1)==1&&value=='B');}
 for(unsigned i=0;i<count;i++)close(installed[i]);close(sv[0]);close(sv[1]);
}
struct receive_thread {int fd;atomic_int ready;ssize_t result;char byte;int rights[253];unsigned count;};
static void *receive_worker(void *arg) {
 struct receive_thread *r=arg;char control[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&r->byte,1};
 struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
 atomic_store(&r->ready,1);r->result=recvmsg(r->fd,&m,0);r->count=r->result>=0?received_fds(&m,r->rights):0;return NULL;
}
static void close_while_waiting(void) {
 alarm(20);int sv[2],src=file_with('W');CHECK("wait_pair",pair(sv)==0);
 struct receive_thread r={.fd=sv[1]};pthread_t thread;CHECK("wait_thread",pthread_create(&thread,NULL,receive_worker,&r)==0);
 while(!atomic_load(&r.ready))usleep(1000);usleep(100000);
 CHECK("wait_close_slot",close(sv[1])==0);int replacement=open("/dev/null",O_RDONLY);CHECK("wait_reuse_slot",replacement==sv[1]);
 CHECK("wait_send_right",send_fds(sv[0],&src,1,'W')==1);close(src);
 CHECK("wait_join",pthread_join(thread,NULL)==0);CHECK("wait_pinned_payload",r.result==1&&r.byte=='W');CHECK("wait_pinned_right",r.count==1);
 if(r.count){char value=0;CHECK("wait_right_content",read(r.rights[0],&value,1)==1&&value=='W');}
 for(unsigned i=0;i<r.count;i++)close(r.rights[i]);close(replacement);close(sv[0]);alarm(0);
}
static void normal_cases(void) {
 int sv[2],src=file_with('P');CHECK("normal_pair",pair(sv)==0);
 int rights[2]={src,src};CHECK("normal_send",send_fds(sv[0],rights,2,'P')==1);
 char byte=0,control[CMSG_SPACE(2*sizeof(int))]={0};int installed[253];struct iovec io={&byte,1};
 struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=CMSG_LEN(sizeof(int))};
 CHECK("short_receive",recvmsg(sv[1],&m,MSG_PEEK|MSG_CMSG_CLOEXEC)==1);
 unsigned count=received_fds(&m,installed);CHECK("short_rights",count==1);CHECK("short_ctrunc",m.msg_flags&MSG_CTRUNC);
 for(unsigned i=0;i<count;i++)close(installed[i]);
 m.msg_controllen=sizeof control;memset(control,0,sizeof control);CHECK("peek_not_consumed",recvmsg(sv[1],&m,MSG_CMSG_CLOEXEC)==1);
 count=received_fds(&m,installed);CHECK("complete_rights",count==2);CHECK("complete_not_truncated",!(m.msg_flags&MSG_CTRUNC));
 close(src);if(count){char content=0;CHECK("retained_after_sender_close",read(installed[0],&content,1)==1&&content=='P');CHECK("description_shared_offset",lseek(installed[1],0,SEEK_CUR)==1);}
 for(unsigned i=0;i<count;i++)close(installed[i]);
 src=file_with('D');CHECK("discard_send",send_fds(sv[0],&src,1,'D')==1);close(src);
 m.msg_control=NULL;m.msg_controllen=0;CHECK("discard_payload",recvmsg(sv[1],&m,0)==1&&byte=='D');CHECK("discard_ctrunc",m.msg_flags&MSG_CTRUNC);
 close(sv[0]);close(sv[1]);
}
static void iovec_width(void) {
 int sv[2];CHECK("iov64_pair",pair(sv)==0);char byte='I';struct iovec io={&byte,1};
 struct msghdr m={.msg_iov=&io};
 // musl exposes a 32-bit member plus ABI padding, unlike glibc's size_t.
 // Populate the actual 64-bit AArch64 kernel slot and bypass libc repacking.
 uint64_t huge=0x100000001ULL;
 memcpy((char*)&m+offsetof(struct msghdr,msg_iovlen),&huge,sizeof huge);
 errno=0;CHECK("iov64_send_overflow",syscall(SYS_sendmsg,sv[0],&m,MSG_DONTWAIT)==-1&&errno==EMSGSIZE);
 errno=0;CHECK("iov64_recv_overflow",syscall(SYS_recvmsg,sv[1],&m,MSG_DONTWAIT)==-1&&errno==EMSGSIZE);
 int regular=open("/dev/null",O_RDONLY);CHECK("nonsocket_open",regular>=0);m.msg_iovlen=1;
 errno=0;CHECK("nonsocket_send_errno",sendmsg(regular,&m,0)==-1&&errno==ENOTSOCK);
 errno=0;CHECK("nonsocket_recv_errno",recvmsg(regular,&m,0)==-1&&errno==ENOTSOCK);close(regular);
 close(sv[0]);close(sv[1]);
}
static void one_case(void (*fn)(void)) {
 pid_t p=fork();if(p<0)exit(93);
 if(!p){checks=failures=0;fn();printf("SCM_CASE checks=%u failures=%u\n",checks,failures);fflush(stdout);_exit(failures?1:0);}
 int status;waitpid(p,&status,0);checks++;if(!WIFEXITED(status)||WEXITSTATUS(status)){failures++;printf("SCM_CASE_CHILD_FAIL status=%d\n",status);}
}
static void one_slot(void){limit_case(1);}static void no_slots(void){limit_case(0);}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IONBF,0);
 if(argc==2&&!strcmp(argv[1],"--host-fds")){
  quiet=1;printf("HOST_FD_BEFORE\n");usleep(500000);
  for(unsigned i=0;i<256;i++)fault_then_receive();
  printf("HOST_FD_AFTER\n");usleep(500000);printf("SCM_STRESS iterations=256 checks=%u failures=%u\n",checks,failures);return failures?1:0;
 }
 one_case(normal_cases);one_case(one_slot);one_case(no_slots);one_case(fault_then_receive);one_case(zero_send);one_case(close_while_waiting);one_case(iovec_width);
 printf("SCM_CONTRACT cases=%u failures=%u\n",checks,failures);return failures?1:0;
}
