#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <uv.h>
#include "octane.h"
#include "socket_listener.h"

#ifdef PLATFORM_POSIX
#include <signal.h>
#endif // PLATFORM_POSIX

uv_loop_t* listener_event_loops;
uv_async_t* listener_async_handles;

void reuseport_thread_start(void *arg);

int uv_multi_listen(const char* address, int port, bool tcp_nodelay, unsigned int threads,
                    enum dispatch_type dispatcher, uv_loop_t* loop, int backlog,
                    void* data, uv_connection_cb cb) {
    #ifdef UNIX
        signal(SIGPIPE, SIG_IGN);
    #endif // UNIX

    listener_event_loops = calloc(threads, sizeof(uv_loop_t));
    listener_async_handles = calloc(threads, sizeof(uv_async_t));

    uv_async_t* service_handle = 0;
    service_handle = malloc(sizeof(uv_async_t));
    uv_async_init(loop, service_handle, NULL);

    service_handle->data = data;

    if (dispatcher == DISPATCH_TYPE_IPC) {
         //TODO: Implement IPC based dispatcher.
    } else if (dispatcher == DISPATCH_TYPE_REUSEPORT) {
        struct socket_listener* listeners;
        listeners = calloc(threads, sizeof(listeners[0]));

        for (int i = 0; i < threads; i++)
        {
            struct socket_listener* listener = listeners + i;
            listener->index = i;
            listener->address = address;
            listener->port = port;
            listener->tcp_nodelay = tcp_nodelay;
            listener->listen_backlog = backlog;
            listener->connection_cb = cb;
            listener->data = data;
            int rc = uv_thread_create(&listener->thread_id, reuseport_thread_start, listener);
        }

        printf("Listening...\n");
        uv_run(loop, UV_RUN_DEFAULT);
    }
    return 0;
}

void reuseport_thread_start(void *arg)
{
#ifdef UNIX
    signal(SIGPIPE, SIG_IGN);
#endif // UNIX

    int rc;
    struct socket_listener* s_listener;
    uv_loop_t* loop;
    uv_tcp_t svr;

    s_listener = arg;
    loop = uv_loop_new();
    listener_event_loops[s_listener->index] = *loop;

    struct sockaddr_in addr;
    uv_tcp_t server;

    rc = uv_tcp_init_ex(loop, &server, AF_INET);
    uv_ip4_addr(s_listener->address, s_listener->port, &addr);

    uv_os_fd_t fd;
    int on = 1;
    uv_fileno(&server, &fd);
    rc = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (char*)&on, sizeof(on));
    if (rc != 0)
    {
        printf("%d\n", errno);
    }

    server.data = s_listener->data;

    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    int r = uv_listen((uv_stream_t*) &server, s_listener->listen_backlog, s_listener->connection_cb);

    rc = uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_delete(loop);
}
