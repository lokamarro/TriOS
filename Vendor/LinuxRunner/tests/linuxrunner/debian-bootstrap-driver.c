#include "app/LinuxRunner/DebianBootstrap.h"
#include <stdio.h>
#include <stdlib.h>
static int calls;
static int cancel(void *p){(void)p;const char *n=getenv("LR_BOOTSTRAP_CANCEL_AFTER");return n&&++calls>=atoi(n);}
static void log_line(void *p,int severity,const char *message){(void)p;fprintf(stderr,"BOOTSTRAP_DIAGNOSTIC severity=%d %s\n",severity,message);}
int main(int argc,char **argv){
 char error[2048]={0};if(argc<3)return 2;
 struct lr_debian_bootstrap_options options={.cancel=cancel,.log=log_line,.trace_entries=getenv("LR_BOOTSTRAP_TRACE")!=NULL};
 int r=lr_debian_bootstrap_diagnosed((const char *const *)argv+2,argc-2,argv[1],&options,error,sizeof error);
 if(r)fprintf(stderr,"BOOTSTRAP_RESULT code=%d %s\n",r,error);return r?1:0;
}
