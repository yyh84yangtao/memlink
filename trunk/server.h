#ifndef MEMLINK_SERVER_H
#define MEMLINK_SERVER_H

#include <stdio.h>
#include <pthread.h>
#include "queue.h"

#define MEMLINK_MAX_THREADS 16

typedef struct _thread_server
{
    pthread_t           threadid;
    struct event_base   *base;
    struct event        notify_event; 
    int                 notify_recv_fd;
    int                 notify_send_fd;
    Queue               *cq;
}ThreadServer;

typedef struct _main_server
{
    int                 sock; 
    struct event_base   *base;
    struct event        event;
    ThreadServer        threads[MEMLINK_MAX_THREADS];
    int                 lastth; // last thread for dispath
}MainServer;

MainServer*     mainserver_create();
void            mainserver_destroy(MainServer *ms);
void            mainserver_loop(MainServer *ms);

int             thserver_init(ThreadServer *ts);

#endif