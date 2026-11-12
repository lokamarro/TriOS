#define _GNU_SOURCE
// Linux asm-generic/AArch64 termios2 ABI used by current Arch glibc isatty.
// Keep the legacy 36-byte termios ABI distinct from the 44-byte termios2 ABI.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
struct lr_termios { uint32_t i,o,c,l; uint8_t line, cc[19]; };
struct lr_termios2 { struct lr_termios t; uint32_t ispeed, ospeed; };
_Static_assert(sizeof(struct lr_termios)==36, "termios ABI");
_Static_assert(sizeof(struct lr_termios2)==44, "termios2 ABI");
#define GET2 0x802c542aUL
#define SET2 0x402c542bUL
#define SETW2 0x402c542cUL
#define SETF2 0x402c542dUL
#define BAUD 0x100fU
#define IBAUD (BAUD << 16)
#define OTHER 0x1000U
static int failures;
#define CHECK(x) do { if(!(x)){printf("FAIL line=%d expression=%s errno=%d\n",__LINE__,#x,errno);failures++;} }while(0)
int main(void) {
    int master=posix_openpt(O_RDWR|O_NOCTTY); CHECK(master>=0);
    CHECK(grantpt(master)==0);CHECK(unlockpt(master)==0);
    char *name=ptsname(master);CHECK(name!=NULL);if(!name)return 1;
    int slave=open(name,O_RDWR|O_NOCTTY);CHECK(slave>=0);
    struct lr_termios2 initial={0},t={0},got={0};
    int rc=ioctl(slave,GET2,&initial);CHECK(rc==0);
    if(rc<0){close(slave);close(master);puts("TERMIOS2_CONTRACT FAIL unsupported TCGETS2");return 1;}
    CHECK(initial.ispeed==38400 && initial.ospeed==38400);
    CHECK(isatty(slave)==1);
    CHECK(ioctl(slave,GET2,NULL)==-1 && errno==EFAULT);
    CHECK(ioctl(slave,SET2,NULL)==-1 && errno==EFAULT);
    t=initial;t.t.c=(t.t.c&~(BAUD|IBAUD))|OTHER|(OTHER<<16);t.ispeed=12345;t.ospeed=23456;
    CHECK(ioctl(master,SET2,&t)==0);CHECK(ioctl(slave,GET2,&got)==0);
    CHECK(memcmp(&t,&got,sizeof t)==0);
    CHECK(ioctl(slave,SETW2,&t)==0);CHECK(ioctl(slave,SETF2,&t)==0);
    struct lr_termios legacy=t.t;legacy.c=(legacy.c&~(BAUD|IBAUD))|13U; // B9600; input follows output
    CHECK(ioctl(slave,0x5402,&legacy)==0);CHECK(ioctl(master,GET2,&got)==0);
    CHECK(got.ispeed==9600 && got.ospeed==9600);
    CHECK(ioctl(slave,0x5401,&legacy)==0);CHECK(legacy.c==got.t.c);
    CHECK(ioctl(slave,SET2,&initial)==0);
    int pipefd[2];CHECK(pipe(pipefd)==0);
    CHECK(ioctl(pipefd[0],GET2,&got)==-1 && errno==ENOTTY);
    close(pipefd[0]);close(pipefd[1]);close(slave);close(master);
    printf("TERMIOS2_CONTRACT %s\n",failures?"FAIL":"PASS");return failures!=0;
}
