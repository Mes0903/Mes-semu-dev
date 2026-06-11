#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "gdbstub.h"

static void require_bool(const char *name, bool got, bool want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? "true" : "false",
            want ? "true" : "false");
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static bool abort_now(void *opaque)
{
    int *calls = opaque;

    (*calls)++;
    return true;
}

static void test_init_interruptible_aborts_idle_accept(void)
{
    char sock_path[96];
    gdbstub_t gdbstub;
    struct target_ops ops = {0};
    arch_info_t arch = {0};
    int abort_calls = 0;

    snprintf(sock_path, sizeof(sock_path), "/tmp/semu-gdbstub-cancel-%ld.sock",
             (long) getpid());
    unlink(sock_path);

    require_bool("interruptible init aborts before client connect",
                 gdbstub_init_interruptible(&gdbstub, &ops, arch, sock_path,
                                            abort_now, &abort_calls),
                 false);
    require_bool("interruptible init checked abort", abort_calls > 0, true);
    unlink(sock_path);
}

static void test_conn_recv_interruptible_aborts_idle_packet_wait(void)
{
    int sv[2];
    conn_t conn;
    int abort_calls = 0;

    require_int("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    memset(&conn, 0, sizeof(conn));
    conn.listen_fd = -1;
    conn.socket_fd = sv[0];
    require_bool("packet buffer init", pktbuf_init(&conn.pktbuf), true);

    require_bool("interruptible recv aborts idle packet wait",
                 conn_recv_packet_interruptible(&conn, abort_now, &abort_calls),
                 false);
    require_bool("interruptible recv checked abort", abort_calls > 0, true);

    close(sv[1]);
    conn_close(&conn);
}

int main(void)
{
    test_init_interruptible_aborts_idle_accept();
    test_conn_recv_interruptible_aborts_idle_packet_wait();
    return 0;
}
