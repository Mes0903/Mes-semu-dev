#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "semu-event.h"

static void require_true(const char *name, bool got)
{
    if (got)
        return;

    fprintf(stderr, "%s: got false, want true\n", name);
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void require_token(const char *name,
                          semu_event_token_t got,
                          semu_event_token_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %u, want %u\n", name, got, want);
    exit(1);
}

static void require_fd_closed_or_negative(const char *name, int fd)
{
    if (fd < 0)
        return;

    errno = 0;
    require_int(name, fcntl(fd, F_GETFD), -1);
    require_int("closed fd errno", errno, EBADF);
}

static volatile sig_atomic_t got_alarm;

static void handle_alarm(int sig UNUSED)
{
    got_alarm = 1;
}

static void test_reserved_tokens(void)
{
    require_token("pause token", SEMU_EVENT_TOKEN_PAUSE, 0);
    require_token("kill token", SEMU_EVENT_TOKEN_KILL, 1);
    require_token("reset token", SEMU_EVENT_TOKEN_RESET, 2);
    require_token("queue pending token", SEMU_EVENT_TOKEN_QUEUE_PENDING, 3);
    require_token("first device token", SEMU_EVENT_TOKEN_FIRST_DEVICE, 16);
}

static void test_notifier_signal_drain_destroy(void)
{
    struct semu_event_notifier notifier = {0};

    require_int("notifier init", semu_event_notifier_init(&notifier), 0);
    require_true("read fd initialized", notifier.read_fd >= 0);
    require_true("write fd initialized", notifier.write_fd >= 0);

    require_int("signal first", semu_event_notifier_signal(&notifier), 0);
    require_int("signal second coalesces",
                semu_event_notifier_signal(&notifier), 0);
    require_int("drain pending", semu_event_notifier_drain(&notifier), 0);
    require_int("drain empty", semu_event_notifier_drain(&notifier), 0);

    int read_fd = notifier.read_fd;
    int write_fd = notifier.write_fd;
    semu_event_notifier_destroy(&notifier);
    require_int("destroy clears read fd", notifier.read_fd, -1);
    require_int("destroy clears write fd", notifier.write_fd, -1);
    require_fd_closed_or_negative("read fd closed", read_fd);
    require_fd_closed_or_negative("write fd closed", write_fd);

    semu_event_notifier_destroy(&notifier);

    struct semu_event_notifier zeroed = {0};
    semu_event_notifier_destroy(&zeroed);
}

static void test_loop_waits_for_notifier_with_token(void)
{
    struct semu_event_loop loop;
    struct semu_event_notifier notifier = {0};
    struct semu_event event = {0};

    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);
    require_int("notifier init", semu_event_notifier_init(&notifier), 0);
    require_int(
        "add notifier read fd",
        semu_event_add_fd(&loop, notifier.read_fd, 42, SEMU_EVENT_READABLE), 0);

    require_int("timeout before signal", semu_event_wait(&loop, &event, 1, 0),
                0);
    require_int("signal notifier", semu_event_notifier_signal(&notifier), 0);
    require_int("wait readable", semu_event_wait(&loop, &event, 1, 100), 1);
    require_token("readable token", event.token, 42);
    require_u32("readable event", event.events, SEMU_EVENT_READABLE);
    require_int("drain notifier", semu_event_notifier_drain(&notifier), 0);
    require_int("timeout after drain", semu_event_wait(&loop, &event, 1, 0), 0);

    semu_event_notifier_destroy(&notifier);
    semu_event_loop_destroy(&loop);
}

static void test_mod_fd_changes_token_and_events(void)
{
    struct semu_event_loop loop;
    int fds[2] = {-1, -1};
    struct semu_event event = {0};

    require_int("pipe", pipe(fds), 0);
    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);
    require_int("add write fd",
                semu_event_add_fd(&loop, fds[1], 10, SEMU_EVENT_WRITABLE), 0);
    require_int("wait writable", semu_event_wait(&loop, &event, 1, 100), 1);
    require_token("initial token", event.token, 10);
    require_u32("initial event", event.events, SEMU_EVENT_WRITABLE);

    require_int("mod read fd to read end missing",
                semu_event_mod_fd(&loop, fds[0], 11, SEMU_EVENT_READABLE),
                -ENOENT);
    require_int("mod write fd token",
                semu_event_mod_fd(&loop, fds[1], 11, SEMU_EVENT_WRITABLE), 0);
    memset(&event, 0, sizeof(event));
    require_int("wait writable after mod",
                semu_event_wait(&loop, &event, 1, 100), 1);
    require_token("modified token", event.token, 11);
    require_u32("modified event", event.events, SEMU_EVENT_WRITABLE);

    semu_event_loop_destroy(&loop);
    close(fds[0]);
    close(fds[1]);
}

static void test_del_fd_removes_registration(void)
{
    struct semu_event_loop loop;
    struct semu_event_notifier notifier = {0};
    struct semu_event event = {0};

    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);
    require_int("notifier init", semu_event_notifier_init(&notifier), 0);
    require_int(
        "add notifier",
        semu_event_add_fd(&loop, notifier.read_fd, 77, SEMU_EVENT_READABLE), 0);
    require_int("del notifier", semu_event_del_fd(&loop, notifier.read_fd), 0);
    require_int("del notifier missing",
                semu_event_del_fd(&loop, notifier.read_fd), -ENOENT);
    require_int("signal after del", semu_event_notifier_signal(&notifier), 0);
    require_int("wait after del times out",
                semu_event_wait(&loop, &event, 1, 0), 0);

    semu_event_notifier_destroy(&notifier);
    semu_event_loop_destroy(&loop);
}


static void test_capacity_overflow_is_rejected(void)
{
    struct semu_event_loop loop;
    int fds[SEMU_EVENT_LOOP_MAX_FDS + 1U][2];

    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS + 1U; i++) {
        fds[i][0] = -1;
        fds[i][1] = -1;
    }

    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);

    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS + 1U; i++)
        require_int("capacity pipe", pipe(fds[i]), 0);

    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS; i++) {
        require_int("fill capacity",
                    semu_event_add_fd(&loop, fds[i][0], (semu_event_token_t) i,
                                      SEMU_EVENT_READABLE),
                    0);
    }

    require_int("capacity overflow",
                semu_event_add_fd(&loop, fds[SEMU_EVENT_LOOP_MAX_FDS][0], 99,
                                  SEMU_EVENT_READABLE),
                -ENOSPC);

    semu_event_loop_destroy(&loop);
    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS + 1U; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
}


static void test_hup_reports_readable_and_error(void)
{
    struct semu_event_loop loop;
    int fds[2] = {-1, -1};
    struct semu_event event = {0};

    require_int("pipe", pipe(fds), 0);
    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);
    require_int("add read fd",
                semu_event_add_fd(&loop, fds[0], 31, SEMU_EVENT_READABLE), 0);

    close(fds[1]);
    fds[1] = -1;

    require_int("wait hup", semu_event_wait(&loop, &event, 1, 100), 1);
    require_token("hup token", event.token, 31);
    require_u32("hup events", event.events,
                SEMU_EVENT_READABLE | SEMU_EVENT_ERROR);

    semu_event_loop_destroy(&loop);
    close(fds[0]);
}

static void test_wait_retries_eintr_and_times_out(void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};
    struct sigaction old_action;
    struct sigaction action;
    struct itimerval timer;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_alarm;
    sigemptyset(&action.sa_mask);

    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 1000;

    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);
    require_int("sigaction install", sigaction(SIGALRM, &action, &old_action),
                0);
    got_alarm = 0;
    require_int("arm timer", setitimer(ITIMER_REAL, &timer, NULL), 0);

    require_int("wait retries EINTR", semu_event_wait(&loop, &event, 1, 20), 0);
    require_true("alarm delivered", got_alarm != 0);

    memset(&timer, 0, sizeof(timer));
    require_int("disarm timer", setitimer(ITIMER_REAL, &timer, NULL), 0);
    require_int("sigaction restore", sigaction(SIGALRM, &old_action, NULL), 0);
    semu_event_loop_destroy(&loop);
}

static void test_invalid_fd_duplicate_mask_and_cap_zero(void)
{
    struct semu_event_loop loop;
    struct semu_event_notifier notifier = {0};

    require_int("loop init", semu_event_loop_init(&loop, "test-loop"), 0);
    require_int("notifier init", semu_event_notifier_init(&notifier), 0);

    require_int("add invalid fd",
                semu_event_add_fd(&loop, -1, 1, SEMU_EVENT_READABLE), -EINVAL);
    require_int("add zero mask",
                semu_event_add_fd(&loop, notifier.read_fd, 1, 0), -EINVAL);
    require_int(
        "add unknown mask",
        semu_event_add_fd(&loop, notifier.read_fd, 1, UINT32_C(1) << 31),
        -EINVAL);
    require_int(
        "add valid",
        semu_event_add_fd(&loop, notifier.read_fd, 1, SEMU_EVENT_READABLE), 0);
    require_int(
        "add duplicate",
        semu_event_add_fd(&loop, notifier.read_fd, 2, SEMU_EVENT_READABLE),
        -EEXIST);
    require_int("mod zero mask",
                semu_event_mod_fd(&loop, notifier.read_fd, 2, 0), -EINVAL);
    require_int(
        "mod unknown mask",
        semu_event_mod_fd(&loop, notifier.read_fd, 2, UINT32_C(1) << 31),
        -EINVAL);
    require_int("del invalid fd", semu_event_del_fd(&loop, -1), -EINVAL);
    require_int("wait cap zero", semu_event_wait(&loop, NULL, 0, 0), -EINVAL);

    semu_event_notifier_destroy(&notifier);
    semu_event_loop_destroy(&loop);
}

int main(void)
{
    test_reserved_tokens();
    test_notifier_signal_drain_destroy();
    test_loop_waits_for_notifier_with_token();
    test_mod_fd_changes_token_and_events();
    test_del_fd_removes_registration();
    test_capacity_overflow_is_rejected();
    test_hup_reports_readable_and_error();
    test_wait_retries_eintr_and_times_out();
    test_invalid_fd_duplicate_mask_and_cap_zero();
    return 0;
}
