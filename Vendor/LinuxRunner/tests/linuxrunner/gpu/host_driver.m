#import "app/LinuxRunner/GuestMetalBackend.h"
extern int lr_cli_main(int,char *const[]);
int main(int argc,char *const argv[]) { @autoreleasepool { LRInstallGuestMetalBackend();return lr_cli_main(argc,argv); } }
