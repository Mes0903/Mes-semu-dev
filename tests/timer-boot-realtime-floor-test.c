#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "utils.h"

static void sleep_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

int main(void)
{
    semu_timer_t timer;

    semu_timer_init(&timer, 1000000, 2);
    uint64_t before = semu_timer_get(&timer);
    sleep_ms(30);
    uint64_t after = semu_timer_get(&timer);
    uint64_t elapsed = after - before;

    if (elapsed < 10000) {
        fprintf(stderr, "boot timer advanced %llu ticks, want at least 10000\n",
                (unsigned long long) elapsed);
        return 1;
    }

    return 0;
}
