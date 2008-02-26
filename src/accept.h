
#ifndef ACCEPT_H
#define ACCEPT_H

#include <pthread.h>
#include <semaphore.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int accept_reconfiguring;
extern int accept_shutting_down;

extern void accept_thread(void *asdf);

extern int connections_accepted;

#endif
