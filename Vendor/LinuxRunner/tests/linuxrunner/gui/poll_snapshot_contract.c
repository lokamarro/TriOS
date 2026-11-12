#define _GNU_SOURCE
/* Exercise a real blocked ppoll, not a fabricated register record. */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void){
 alarm(15);char logpath[]="snapshot-poll-contract.XXXXXX";int logfd=mkstemp(logpath);if(logfd<0)return 1;close(logfd);int p[2],ready[2];if(pipe(p)||pipe(ready))return 1;
 pid_t waiter=fork();if(waiter<0)return 1;
 if(!waiter){close(ready[0]);if(write(ready[1],"R",1)!=1)_exit(2);struct pollfd f={.fd=p[0],.events=POLLIN};ppoll(&f,1,NULL,NULL);_exit(3);}
 close(ready[1]);char byte;if(read(ready[0],&byte,1)!=1)return 1;usleep(50000);
 pid_t observer=fork();if(observer<0)return 1;
 if(!observer){execl("./runtime-snapshot","runtime-snapshot",logpath,NULL);_exit(4);}
 int status;if(waitpid(observer,&status,0)!=observer)return 1;
 kill(waiter,SIGTERM);if(waitpid(waiter,NULL,0)!=waiter)return 1;
 if(!WIFEXITED(status)||WEXITSTATUS(status))return 1;
 FILE *in=fopen(logpath,"r");if(!in)return 1;
 char line[512],expected[128];int descriptor=0,infinite=0;
 snprintf(expected,sizeof expected,"POLL_DESCRIPTOR tid=%d fd=%d events=0x1",waiter,p[0]);
 while(fgets(line,sizeof line,in)){
  if(strstr(line,expected))descriptor=1;
  char timeout[96];snprintf(timeout,sizeof timeout,"POLL_TIMEOUT tid=%d infinite=1",waiter);
  if(strstr(line,timeout))infinite=1;
 }
 fclose(in);close(p[0]);close(p[1]);close(ready[0]);
 if(!descriptor||!infinite){fprintf(stderr,"POLL_SNAPSHOT_FAIL descriptor=%d infinite=%d\n",descriptor,infinite);return 1;}
 puts("POLL_SNAPSHOT_PASS actual blocked descriptor and infinite timeout captured without payload");return 0;
}
