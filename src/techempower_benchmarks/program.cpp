#include <stdio.h>
#include <uv.h>
#include <stdlib.h>
#include <stdbool.h>
#include <octane.h>
#include <time.h>
#include "common.h"
#include "connection.hpp"
#include "responders/sds_responder.hpp"

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

void stream_on_connect(uv_stream_t* stream, int status);
void stream_on_alloc(uv_handle_t* client, size_t suggested_size, uv_buf_t* buf);
void stream_on_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf);
void timer_callback(uv_timer_t* timer);

void on_new_connection(http_connection* connection, uv_stream_t* server, int status);
void on_alloc(http_connection* connection, uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
void on_read(http_connection* connection, uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
void on_request(http_connection* connection, http_request** requests, int number_of_requests);
void (*stream_on_read_func)(connection* conn, size_t requests, uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

char* current_time;
uv_timer_t timer;

int main(int argc, char *argv[]) {
    http_listener* listener = new_http_listener();
    uv_timer_init(listener->loop, &timer);
    uv_timer_start(&timer, timer_callback, 0, 500);

    begin_listening(listener, "0.0.0.0", 8000, false, 4, 128, NULL, NULL, NULL, on_request);

    printf("Listening...\n");
}

void on_new_connection(http_connection* connection, uv_stream_t* server, int status) {
    //printf("NEW CONNECTION\n");
}

void on_alloc(http_connection* connection, uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    //printf("ALLOCATING!\n");
}

void on_read(http_connection* connection, uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    //printf("READING!\n");
}

void on_request(http_connection* connection, http_request** requests, int number_of_requests) {
    //printf("method is %s\n", requests[0].method);
    //printf("path is %s\n", requests[0].path);
    //printf("HTTP version is 1.%d\n", requests[0].version);
    //printf("headers:\n");
    //for (int i = 0; i != num_headers; ++i) {
    //    printf("%.*s: %.*s\n", (int) headers[i].name_len, headers[i].name,
    //           (int) headers[i].value_len, headers[i].value);
    //}

    write_batch* batch = create_write_batch(number_of_requests);

    for (int i=0; i<number_of_requests; i++) {
        create_plaintext_response_sds(batch);
    }
    if (http_connection_is_writable(connection)) {
        // TODO: Use the return values from uv_write()
        int rc = http_connection_write(connection, batch);
    } else {
        // TODO: Handle closing the stream.
    }

    free_http_requests(requests, number_of_requests);
}

int main2(int argc, char *argv[]) {
    //stream_on_read_func = &stream_on_read_static;
    stream_on_read_func = &stream_on_read_sds;
    //stream_on_read_func = &stream_on_read_nobuffer;

    uv_async_t* service_handle = 0;
    uv_loop_t* loop = uv_default_loop();

    uv_timer_init(loop, &timer);
    uv_timer_start(&timer, timer_callback, 0, 500);

    uv_multi_listen("0.0.0.0", 8000, false, 40, DISPATCH_TYPE_REUSEPORT, loop, 128,
                    NULL, stream_on_connect);
}

void send_error_response(connection* conn, http_request_state state) {

}

void stream_on_close(uv_handle_t* handle) {
    uv_handle_t* stream = handle;
    connection* conn = (connection*)stream->data;

    if (conn->state != CONNECTION_CLOSED) {
        conn->state = CONNECTION_CLOSED;
        connection* conn = (connection*)handle->data;
        free_connection(conn);
    }
}

void stream_close_connection(connection* conn) {
    if (conn->state == CONNECTION_OPEN) {
        conn->state = CONNECTION_CLOSING;
        uv_close((uv_handle_t*)&conn->stream, stream_on_close);
    }
}

void handle_request_error(connection* conn, http_request_state state) {
    uv_stream_t* stream = (uv_stream_t*)&conn->stream;

    if (conn->state == CONNECTION_OPEN) {
        uv_read_stop(stream);
    }

    conn->keep_alive = false;

    if (conn->state == CONNECTION_OPEN) {
        /* Send the error message back. */
        send_error_response(conn, state);
    } else {
        stream_close_connection(conn);
    }
}

void handle_bad_request(connection* conn) {
    handle_request_error(conn, BAD_REQUEST);
}

void handle_buffer_exceeded_error(connection* conn) {
    handle_request_error(conn, SIZE_EXCEEDED);
}

void handle_internal_error(connection* conn) {
    handle_request_error(conn, INTERNAL_ERROR);
}

void stream_on_shutdown(uv_shutdown_t* req, int status) {
    connection* conn = (connection*)req->data;
    uv_stream_t* stream = (uv_stream_t*)&conn->stream;
    if (conn->state == CONNECTION_OPEN) {
        stream_close_connection(conn);
    }
    free(req);
}

void stream_on_connect(uv_stream_t* server_stream, int status) {
    /* TODO: Use the return values from uv_accept() and uv_read_start() */
    int rc = 0;

    connection* conn = create_connection();
    rc = uv_tcp_init(server_stream->loop, (uv_tcp_t*)&conn->stream);
    conn->bytes_remaining = 0;
    conn->request_length = 0;
    conn->stream.data = conn;

    rc = uv_accept(server_stream, (uv_stream_t*)&conn->stream);
    uv_read_start((uv_stream_t*)&conn->stream, stream_on_alloc, stream_on_read);
}

void stream_on_alloc(uv_handle_t* client, size_t suggested_size, uv_buf_t* buf) {
    char* buffer;
    if(!(buffer = (char*)malloc(suggested_size))){
        memory_error("Unable to allocate buffer of size %d", suggested_size);
    }
    *buf = uv_buf_init(buffer, suggested_size);
}

void stream_on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    connection* conn = (connection*)stream->data;

    if (nread > 0) {
        if (conn->request_length == 0) {
            // We need to seek the first request to find out how many characters each request is.
            for (int i = 1; i < nread; i++) {
                if (buf->base[i] == '\r' && buf->base[i - 1] == '\n') {
                    conn->request_length = i + 2;
                    break;
                }
            }
        }

        ssize_t requests = (nread + conn->bytes_remaining) / conn->request_length;
        conn->bytes_remaining = conn->bytes_remaining + (nread % conn->request_length);

        stream_on_read_func(conn, requests, stream, nread, buf);
    }
    else if (nread == UV_ENOBUFS) {
        handle_buffer_exceeded_error(conn);
    }
    else if (nread == UV_EOF){
        uv_shutdown_t* req = (uv_shutdown_t*)malloc(sizeof(uv_shutdown_t));
        req->data = conn;
        uv_shutdown(req, (uv_stream_t*)&conn->stream, stream_on_shutdown);
    }
    else if (nread == UV_ECONNRESET || nread == UV_ECONNABORTED) {
        /* Let's close the connection as the other peer just disappeared */
        stream_close_connection(conn);
    } else {
        /* We didn't see this coming, but an unexpected UV error code was passed in, so we'll
         * respond with a blanket 500 error if we can */
        handle_internal_error(conn);
    }
    free(buf->base);
}

void timer_callback(uv_timer_t* timer) {
    time_t curtime;
    time(&curtime);
    char* time = ctime(&curtime);
    current_time = time;
}