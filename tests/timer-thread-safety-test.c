#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "utils.h"

#define READER_THREADS 4
#define ITERATIONS 20000

static semu_timer_t timer;

static void *timer_reader(void *arg)
{
    (void) arg;
    uint64_t last = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t now = semu_timer_get(&timer);
        if (now < last) {
            fprintf(stderr, "timer moved backwards: %llu < %llu\n",
                    (unsigned long long) now, (unsigned long long) last);
            abort();
        }
        last = now;
    }

    return NULL;
}

static void *boot_completer(void *arg)
{
    (void) arg;

    for (int i = 0; i < ITERATIONS / 4; i++)
        (void) semu_timer_get(&timer);

    semu_boot_complete_store(true);
    return NULL;
}

int main(void)
{
    pthread_t readers[READER_THREADS];
    pthread_t completer;

    semu_boot_complete_store(false);
    semu_timer_init(&timer, 65000000, READER_THREADS);

    for (int i = 0; i < READER_THREADS; i++) {
        if (pthread_create(&readers[i], NULL, timer_reader, NULL) != 0) {
            perror("pthread_create reader");
            return 1;
        }
    }

    if (pthread_create(&completer, NULL, boot_completer, NULL) != 0) {
        perror("pthread_create completer");
        return 1;
    }

    for (int i = 0; i < READER_THREADS; i++)
        pthread_join(readers[i], NULL);
    pthread_join(completer, NULL);

    return 0;
}
