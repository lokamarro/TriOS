/* SIGPIPE semantics a Chromium/Electron process relies on at shutdown. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
static void check(const char*what,int ok){printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok)fails++;}
static int broken_pipe(void){int p[2];if(pipe(p))return -1;close(p[0]);return p[1];}
static void*writer(void*a){(void)a;int fd=broken_pipe();long r=write(fd,"x",1);long e=errno;close(fd);return (void*)(r==-1&&e==EPIPE?1L:0L);}
static int sigign_bit(void){FILE*f=fopen("/proc/self/status","r");char l[256];unsigned long long m=0;while(f&&fgets(l,sizeof l,f))if(!strncmp(l,"SigIgn:",7))m=strtoull(l+7,0,16);if(f)fclose(f);return (m>>(SIGPIPE-1))&1;}
int main(int argc,char**argv){
 if(argc>1&&!strcmp(argv[1],"child")){int fd=broken_pipe();long r=write(fd,"x",1);printf("%s SIG_IGN kept across execve (write=%ld errno=%d sigign=%d)\n",r==-1&&errno==EPIPE&&sigign_bit()?"PASS":"FAIL",r,errno,sigign_bit());return r==-1&&errno==EPIPE?0:1;}
 pid_t c=fork();if(!c){int fd=broken_pipe();if(write(fd,"x",1)<0)_exit(2);_exit(0);}int st;waitpid(c,&st,0);check("default SIGPIPE kills the writer",WIFSIGNALED(st)&&WTERMSIG(st)==SIGPIPE);
 signal(SIGPIPE,SIG_IGN);
 int fd=broken_pipe();long r=write(fd,"x",1);check("ignored: main-thread write gets EPIPE",r==-1&&errno==EPIPE);close(fd);
 pthread_t t;void*ok;pthread_create(&t,0,writer,0);pthread_join(t,&ok);check("ignored: worker-thread write gets EPIPE",ok==(void*)1L);
 int s[2];socketpair(AF_UNIX,SOCK_STREAM,0,s);close(s[1]);r=send(s[0],"x",1,0);check("ignored: send on closed stream gets EPIPE",r==-1&&errno==EPIPE);
 sigset_t b;sigemptyset(&b);sigaddset(&b,SIGPIPE);signal(SIGPIPE,SIG_DFL);sigprocmask(SIG_BLOCK,&b,0);fd=broken_pipe();r=write(fd,"x",1);sigset_t pend;sigpending(&pend);
 check("blocked: EPIPE and SIGPIPE pending",r==-1&&errno==EPIPE&&sigismember(&pend,SIGPIPE));signal(SIGPIPE,SIG_IGN);sigpending(&pend);check("ignoring discards the pending SIGPIPE",!sigismember(&pend,SIGPIPE));sigprocmask(SIG_UNBLOCK,&b,0);
 c=fork();if(!c){execl("/proc/self/exe",argv[0],"child",(char*)0);_exit(9);}waitpid(c,&st,0);check("exec child exited normally",WIFEXITED(st)&&WEXITSTATUS(st)==0);
 c=fork();if(!c){int s2[2];socketpair(AF_UNIX,SOCK_STREAM,0,s2);close(s2[1]);signal(SIGPIPE,SIG_DFL);long q=send(s2[0],"x",1,MSG_NOSIGNAL);_exit(q==-1&&errno==EPIPE?0:1);}waitpid(c,&st,0);check("MSG_NOSIGNAL: EPIPE without signal",WIFEXITED(st)&&WEXITSTATUS(st)==0);
 printf("SIGPIPE_CONTRACT %s failures=%d\n",fails?"FAIL":"PASS",fails);return fails!=0;}
