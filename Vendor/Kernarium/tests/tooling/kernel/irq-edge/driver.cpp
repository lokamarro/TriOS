// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
extern "C" {
int os_setup_epoll(); int os_add_epoll_fd(int,int,void*);
int os_del_epoll_fd(int); int os_waiting_for_events_epoll(); void os_close_epoll_fd();
void *os_epoll_get_data_pointer(int); int os_epoll_triggered(int,int);
int darwin_irq_set_notification(int,int); int os_irq_wake_pending();
void darwin_irq_idle_begin(sigset_t*); void darwin_irq_idle_end(const sigset_t*);
void panic(const char*,...) { std::abort(); }
void os_set_thread_name(const char*) {}
}
int main() {
 int fds[2]; assert(pipe(fds)==0); int token=1, next=2;
 assert(os_add_epoll_fd(1,fds[0],&token)==0);
 assert(write(fds[1],"a",1)==1);
 assert(os_setup_epoll()==0);
 // A descriptor that notifies and has input is a wake source. A system
 // suspend clears the notification of the others, and setting it restores it.
 assert(os_irq_wake_pending()==1);
 assert(darwin_irq_set_notification(fds[0],0)==0 && os_irq_wake_pending()==0);
 assert(darwin_irq_set_notification(fds[0],1)==0 && os_irq_wake_pending()==1);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_epoll_get_data_pointer(0)==&token && os_epoll_triggered(0,1));
 // Reentry before the outer handler drains must not deliver the same level.
 assert(os_waiting_for_events_epoll()==0);
 // Adding a registered descriptor modifies it and rearms its edge.
 assert(os_add_epoll_fd(1,fds[0],&next)==0);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_epoll_get_data_pointer(0)==&next);
 assert(os_waiting_for_events_epoll()==0);
 char byte; assert(read(fds[0],&byte,1)==1);
 // Drain and refill between polls must still produce a fresh edge.
 assert(write(fds[1],"b",1)==1);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_waiting_for_events_epoll()==0);
 assert(read(fds[0],&byte,1)==1);
 close(fds[1]); assert(os_waiting_for_events_epoll()==1);
 assert(os_waiting_for_events_epoll()==0);
 assert(os_del_epoll_fd(fds[0])==0); close(fds[0]);
 os_close_epoll_fd(); puts("PASS IRQ reentry, MOD rearm, wake source, drain/refill, EOF, early registration, shutdown");
}
