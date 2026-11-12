#import <Foundation/Foundation.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/resource.h>
static atomic_uint polls;
static int measured_poll(struct pollfd *fds,nfds_t n,int t){atomic_fetch_add(&polls,1);return poll(fds,n,t);}
#define poll measured_poll
#ifndef LR_CLIENT_SOURCE
#define LR_CLIENT_SOURCE "app/LinuxRunner/RFBClient.m"
#endif
#include LR_CLIENT_SOURCE
#undef poll
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=2)return 2;LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:@"Test2345"];
 __block BOOL connected=NO;c.statusHandler=^(NSString*s){if([s hasPrefix:@"Connected"])connected=YES;};[c start];
 NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:5];while(!connected&&deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];if(!connected)return 1;
 atomic_store(&polls,0);struct rusage before,after;getrusage(RUSAGE_SELF,&before);NSDate *end=[NSDate dateWithTimeIntervalSinceNow:1];while(end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];getrusage(RUSAGE_SELF,&after);
 printf("idle_poll_calls=%u host_cpu_us=%ld\n",atomic_load(&polls),(after.ru_utime.tv_sec-before.ru_utime.tv_sec+after.ru_stime.tv_sec-before.ru_stime.tv_sec)*1000000L+after.ru_utime.tv_usec-before.ru_utime.tv_usec+after.ru_stime.tv_usec-before.ru_stime.tv_usec);fflush(stdout);
 [c stop];[NSThread sleepForTimeInterval:.15];return 0;
}}
