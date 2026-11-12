#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "kernel/task.h"
#include "fs/poll.h"
int main(void) {
    int saved=dup(STDIN_FILENO);assert(saved>=0);close(STDIN_FILENO);
    struct poll *p=poll_create();assert(!IS_ERR(p));
    assert(fcntl(STDIN_FILENO,F_GETFD)>=0);
    poll_destroy(p);
    assert(fcntl(STDIN_FILENO,F_GETFD)<0);
    assert(dup2(saved,STDIN_FILENO)==STDIN_FILENO);close(saved);
    puts("POLL_HOST_OWNERSHIP_PASS backend-in-stdio-slot-released");
}
