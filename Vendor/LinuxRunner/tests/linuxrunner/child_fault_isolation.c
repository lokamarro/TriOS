#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    alarm(20);int request[2],reply[2];assert(!pipe(request)&&!pipe(reply));
    pid_t sibling=fork();assert(sibling>=0);
    if(!sibling){
        close(request[1]);close(reply[0]);char c;
        while(read(request[0],&c,1)==1){if(c=='q')_exit(23);assert(write(reply[1],&c,1)==1);}
        _exit(1);
    }
    close(request[0]);close(reply[1]);
    for(int i=0;i<2;i++) {
        pid_t victim=fork();assert(victim>=0);
        if(!victim){
            if(i==0)__asm__ volatile(".inst 0x7f409fff" ::: "v31");
            else {
                volatile uint8_t *p=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                if(p==MAP_FAILED)_exit(2);
                (void)*p;
            }
            _exit(3);
        }
        int status;assert(waitpid(victim,&status,0)==victim);
        assert(WIFSIGNALED(status)&&WTERMSIG(status)==(i==0?SIGILL:SIGSEGV));
        assert(waitpid(sibling,&status,WNOHANG)==0);
        char c='a'+i,out;assert(write(request[1],&c,1)==1&&read(reply[0],&out,1)==1&&out==c);
    }
    char q='q';assert(write(request[1],&q,1)==1);int status;
    assert(waitpid(sibling,&status,0)==sibling&&WIFEXITED(status)&&WEXITSTATUS(status)==23);
    close(request[1]);close(reply[0]);
    puts("CHILD_FAULT_ISOLATION_PASS SIGILL=PASS SIGSEGV=PASS parent_alive=PASS sibling_IPC=PASS reaping=PASS");
    return 0;
}
