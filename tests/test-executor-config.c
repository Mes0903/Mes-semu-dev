#include "executor-config.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check_true(bool condition, const char *message)
{
    if (condition)
        return;

    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void test_executor_mode_parse(void)
{
    enum semu_executor_mode mode;

    check_true(semu_executor_mode_parse("single-thread", &mode) == 0,
               "single-thread parses");
    check_true(mode == SEMU_EXECUTOR_SINGLE_THREAD,
               "single-thread maps to single backend mode");
    check_true(semu_executor_mode_parse("threaded-cpu-with-legacy-device-gate",
                                        &mode) == 0,
               "legacy gate parses");
    check_true(mode == SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE,
               "legacy gate maps to gate mode");
    check_true(
        semu_executor_mode_parse("threaded-cpu-with-device-actors", &mode) == 0,
        "actor mode parses");
    check_true(mode == SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS,
               "actor mode maps to actor mode");
    check_true(semu_executor_mode_parse("bad", &mode) == -1,
               "unknown mode is rejected");
    check_true(semu_executor_mode_parse(NULL, &mode) == -1,
               "NULL mode string is rejected");
    check_true(semu_executor_mode_parse("single-thread", NULL) == -1,
               "NULL output mode is rejected");
}

static void test_executor_mode_names(void)
{
    check_true(strcmp(semu_executor_mode_name(SEMU_EXECUTOR_SINGLE_THREAD),
                      "single-thread") == 0,
               "single-thread name is stable");
    check_true(strcmp(semu_executor_mode_name(
                          SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE),
                      "threaded-cpu-with-legacy-device-gate") == 0,
               "gate mode name is stable");
    check_true(strcmp(semu_executor_mode_name(
                          SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS),
                      "threaded-cpu-with-device-actors") == 0,
               "actor mode name is stable");
}

static void test_default_executor_mode(void)
{
    check_true(semu_executor_default_mode(1) == SEMU_EXECUTOR_SINGLE_THREAD,
               "default selector is single-thread for one hart");
    check_true(semu_executor_default_mode(2) ==
                   SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE,
               "default selector is legacy gate for multiple harts");
}

static void test_executor_backend_mapping(void)
{
    check_true(semu_executor_backend_for_mode(SEMU_EXECUTOR_SINGLE_THREAD) ==
                   HART_EXEC_SINGLE_THREAD,
               "single-thread uses single-thread backend");
    check_true(semu_executor_backend_for_mode(
                   SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE) ==
                   HART_EXEC_DEDICATED_THREADS,
               "gate mode uses dedicated threads");
    check_true(semu_executor_backend_for_mode(
                   SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS) ==
                   HART_EXEC_DEDICATED_THREADS,
               "actor mode uses dedicated threads");
}

struct expected_policy {
    const char *name;
    bool build_enabled;
    bool actor_mode_allowed;
};

static void test_virtio_host_io_policy_table(void)
{
    static const struct expected_policy expected[] = {
        {"virtio-gpu", SEMU_HAS(VIRTIOGPU), true},
        {"virtio-input", SEMU_HAS(VIRTIOINPUT), false},
        {"virtio-rng", SEMU_HAS(VIRTIORNG), true},
        {"virtio-blk", SEMU_HAS(VIRTIOBLK), false},
        {"virtio-fs", SEMU_HAS(VIRTIOFS), false},
        {"virtio-net", SEMU_HAS(VIRTIONET), false},
        {"virtio-snd", SEMU_HAS(VIRTIOSND), false},
    };

    check_true(semu_executor_virtio_host_io_policy_count() ==
                   sizeof(expected) / sizeof(expected[0]),
               "VirtIO host I/O policy table has one row per device");

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const struct semu_executor_virtio_host_io_policy *policy =
            semu_executor_virtio_host_io_policy_at(i);

        check_true(policy != NULL, "policy table entry is addressable");
        if (!policy)
            continue;

        check_true(strcmp(policy->device_name, expected[i].name) == 0,
                   "policy table device order is deterministic");
        check_true(policy->build_enabled == expected[i].build_enabled,
                   "policy table records build-enabled state");
        check_true(policy->actor_mode_allowed == expected[i].actor_mode_allowed,
                   "policy table records actor-mode allowlist");
        check_true(policy->actor_mode_policy != NULL &&
                       policy->actor_mode_policy[0] != '\0',
                   "policy table records host I/O policy text");
    }

    check_true(semu_executor_virtio_host_io_policy_at(
                   semu_executor_virtio_host_io_policy_count()) == NULL,
               "policy table rejects out-of-range lookups");
}

static void append_expected_device(char *buffer,
                                   size_t buffer_size,
                                   const char *device_name)
{
    size_t len = strlen(buffer);

    if (len > 0) {
        strncat(buffer, ", ", buffer_size - len - 1);
        len = strlen(buffer);
    }
    strncat(buffer, device_name, buffer_size - len - 1);
}

static void build_expected_unsupported_devices(char *buffer, size_t buffer_size)
{
    buffer[0] = '\0';

    for (size_t i = 0; i < semu_executor_virtio_host_io_policy_count(); i++) {
        const struct semu_executor_virtio_host_io_policy *policy =
            semu_executor_virtio_host_io_policy_at(i);

        if (policy->build_enabled && !policy->actor_mode_allowed)
            append_expected_device(buffer, buffer_size, policy->device_name);
    }
}

static void test_actor_gate_names_legacy_devices(void)
{
    char expected_unsupported[SEMU_EXECUTOR_DEVICE_GATE_UNSUPPORTED_CAP];
    struct semu_executor_device_gate gate =
        semu_executor_check_actor_device_gate(
            SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS);

    build_expected_unsupported_devices(expected_unsupported,
                                       sizeof(expected_unsupported));
    check_true(strcmp(gate.unsupported_devices, expected_unsupported) == 0,
               "actor gate unsupported list follows policy table order");

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIONET) || SEMU_HAS(VIRTIOBLK) || \
    SEMU_HAS(VIRTIOSND) || SEMU_HAS(VIRTIOFS)
    check_true(!gate.allowed, "actor mode rejects legacy devices");
#if SEMU_HAS(VIRTIOBLK)
    check_true(strstr(gate.unsupported_devices, "virtio-blk") != NULL,
               "gate names virtio-blk when enabled");
#endif
#if SEMU_HAS(VIRTIOINPUT)
    check_true(strstr(gate.unsupported_devices, "virtio-input") != NULL,
               "gate names virtio-input when enabled");
#endif
#if SEMU_HAS(VIRTIONET)
    check_true(strstr(gate.unsupported_devices, "virtio-net") != NULL,
               "gate names virtio-net when enabled");
#endif
#if SEMU_HAS(VIRTIORNG)
    check_true(strstr(gate.unsupported_devices, "virtio-rng") == NULL,
               "gate does not name virtio-rng when enabled");
#endif
#if SEMU_HAS(VIRTIOSND)
    check_true(strstr(gate.unsupported_devices, "virtio-snd") != NULL,
               "gate names virtio-snd when enabled");
#endif
#if SEMU_HAS(VIRTIOFS)
    check_true(strstr(gate.unsupported_devices, "virtio-fs") != NULL,
               "gate names virtio-fs when enabled");
#endif
    check_true(
        strstr(gate.fallback_command,
               "--executor=threaded-cpu-with-legacy-device-gate") != NULL,
        "gate includes fallback command");
#else
    check_true(gate.allowed, "actor mode allows actor-ready feature set");
    check_true(strcmp(gate.unsupported_devices, "") == 0,
               "actor-ready feature set has no unsupported devices");
#endif

    gate = semu_executor_check_actor_device_gate(
        SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE);
    check_true(gate.allowed, "gate mode itself is always allowed");
    gate = semu_executor_check_actor_device_gate(SEMU_EXECUTOR_SINGLE_THREAD);
    check_true(gate.allowed, "single-thread mode is always allowed");
}

int main(void)
{
    test_executor_mode_parse();
    test_executor_mode_names();
    test_default_executor_mode();
    test_executor_backend_mapping();
    test_virtio_host_io_policy_table();
    test_actor_gate_names_legacy_devices();

    if (failures != 0) {
        fprintf(stderr, "%d executor config checks failed\n", failures);
        return 1;
    }
    return 0;
}
