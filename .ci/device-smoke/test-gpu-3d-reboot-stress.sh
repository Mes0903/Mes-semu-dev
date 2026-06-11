#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/../common.sh"

export ENABLE_VIRGL="${ENABLE_VIRGL:-1}"
export HEADLESS=0
export DISKIMG_FILE="${DISKIMG_FILE:-test-tools.img}"
export NETDEV="${NETDEV:-user}"
export SMP="${SMP:-2}"
export EXECUTOR="${EXECUTOR:-threaded-cpu-with-device-actors}"

case "${OS_TYPE}" in
    Darwin)
        DEFAULT_BOOT_TIMEOUT=10800
        DEFAULT_CMD_TIMEOUT=600
        DEFAULT_GLXINFO_RETRIES=90
        DEFAULT_XORG_RETRIES=90
        DEFAULT_GLXGEARS_SECONDS=10
        ;;
    *)
        DEFAULT_BOOT_TIMEOUT=300
        DEFAULT_CMD_TIMEOUT=180
        DEFAULT_GLXINFO_RETRIES=45
        DEFAULT_XORG_RETRIES=45
        DEFAULT_GLXGEARS_SECONDS=5
        ;;
esac

export TIMEOUT="${VGPU3D_BOOT_TIMEOUT:-${SEMU_TEST_TIMEOUT:-${DEFAULT_BOOT_TIMEOUT}}}"
export VGPU3D_CMD_TIMEOUT="${VGPU3D_CMD_TIMEOUT:-${DEFAULT_CMD_TIMEOUT}}"
export VGPU3D_GLXINFO_RETRIES="${VGPU3D_GLXINFO_RETRIES:-${DEFAULT_GLXINFO_RETRIES}}"
export VGPU3D_GLXINFO_SLEEP="${VGPU3D_GLXINFO_SLEEP:-1}"
export VGPU3D_XORG_RETRIES="${VGPU3D_XORG_RETRIES:-${DEFAULT_XORG_RETRIES}}"
export VGPU3D_XORG_SLEEP="${VGPU3D_XORG_SLEEP:-1}"
export VGPU3D_GLXGEARS_SECONDS="${VGPU3D_GLXGEARS_SECONDS:-${DEFAULT_GLXGEARS_SECONDS}}"
export VGPU3D_REBOOT_RUNS="${VGPU3D_REBOOT_RUNS:-1}"

case "${VGPU3D_REBOOT_RUNS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_REBOOT_RUNS must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_REBOOT_RUNS < 1 )); then
    print_error "FAIL: VGPU3D_REBOOT_RUNS must be a positive integer"
    exit 1
fi

case "${VGPU3D_GLXGEARS_SECONDS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_GLXGEARS_SECONDS must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_GLXGEARS_SECONDS < 1 )); then
    print_error "FAIL: VGPU3D_GLXGEARS_SECONDS must be a positive integer"
    exit 1
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    print_error "FAIL: visible vgpu 3D reboot stress needs host DISPLAY or WAYLAND_DISPLAY"
    exit 1
fi

if ! command -v sdl2-config >/dev/null 2>&1; then
    print_error "FAIL: visible vgpu 3D reboot stress needs sdl2-config in PATH"
    exit 1
fi

case "${ENABLE_VIRGL}" in
    1|true|yes)
        if ! command -v pkg-config >/dev/null 2>&1 ||
           ! pkg-config --exists virglrenderer epoxy gl egl; then
            print_error "FAIL: visible vgpu 3D reboot stress needs pkg-config packages: virglrenderer epoxy gl egl"
            exit 1
        fi
        ;;
esac

cleanup
trap cleanup EXIT

echo "Running vgpu 3D reboot stress: ENABLE_VIRGL=${ENABLE_VIRGL} HEADLESS=${HEADLESS} DISKIMG_FILE=${DISKIMG_FILE} NETDEV=${NETDEV} SMP=${SMP} EXECUTOR=${EXECUTOR} VGPU3D_REBOOT_RUNS=${VGPU3D_REBOOT_RUNS} VGPU3D_GLXGEARS_SECONDS=${VGPU3D_GLXGEARS_SECONDS}"

# Feature toggles are passed through environment variables, which do not
# participate in make's normal dependency tracking. Force a rebuild here so
# visible virgl actor/reboot runs never reuse a stale semu binary or DTB.
make -B semu minimal.dtb

if [ ! -f Image ] || [ ! -f rootfs.cpio ]; then
    make Image rootfs.cpio
fi
if [ ! -f test-tools.img ]; then
    make test-tools.img
fi
if [[ "${DISKIMG_FILE}" != "test-tools.img" && ! -f "${DISKIMG_FILE}" ]]; then
    print_error "FAIL: DISKIMG_FILE not found: ${DISKIMG_FILE}"
    exit 1
fi

# NOTE: Capture expect's exit code so the shell wrapper can print a concise
# stage-specific message while expect prints detailed guest-side diagnostics.
set +e
expect <<'DONE'
proc send_cmd {cmd exit_code} {
  global timeout
  expect "# " { send -- "$cmd\r" } timeout { exit $exit_code }
}

proc expect_status {ok bad exit_code} {
  global timeout
  expect -exact $ok {} -exact $bad { exit $exit_code } timeout { exit $exit_code }
}

proc login_guest {exit_code} {
  global env timeout

  set timeout $env(TIMEOUT)
  expect "buildroot login:" { send "root\r" } timeout { exit $exit_code }
  expect "# "              { send "uname -a\r" } timeout { exit 2 }
  expect "riscv32 GNU/Linux" {}
  set timeout $env(VGPU3D_CMD_TIMEOUT)
}

proc run_compact_probe {label} {
  global env timeout

  set timeout $env(VGPU3D_CMD_TIMEOUT)

  puts "--- vgpu 3D compact probe: $label ---"

  send_cmd {ls -la /dev/dri/ 2>/dev/null || true} 3
  send_cmd {if test -c /dev/dri/card0 && test -c /dev/dri/renderD128; then status=OK; else status=MISSING; fi; printf "__VGPU_DRM_%s__\n" "$status"} 3
  expect_status "__VGPU_DRM_OK__" "__VGPU_DRM_MISSING__" 3

  send_cmd {if ls /sys/bus/virtio/drivers/virtio_gpu/virtio* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf "__VGPU_BIND_%s__\n" "$status"} 3
  expect {
    -exact "__VGPU_BIND_OK__" {}
    -exact "__VGPU_BIND_BAD__" {
      send -- "ls -l /sys/bus/virtio/drivers/virtio_gpu/ 2>/dev/null || true\r"
      send -- "for d in /sys/bus/virtio/devices/virtio*; do echo \$d; ls -l \$d/driver 2>/dev/null || true; done\r"
      exit 3
    }
    timeout { exit 3 }
  }

  send_cmd {dmesg > /tmp/vgpu3d-dmesg.log; grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' /tmp/vgpu3d-dmesg.log | tail -n 120 || true} 4
  send_cmd {status=OK; grep -Fqi '+virgl' /tmp/vgpu3d-dmesg.log || status=BAD; grep -Fqi '+resource_blob' /tmp/vgpu3d-dmesg.log || status=BAD; grep -Fqi '+host_visible' /tmp/vgpu3d-dmesg.log || status=BAD; grep -Eiq 'number of cap sets: *[1-9]|cap set.*id *1' /tmp/vgpu3d-dmesg.log || status=BAD; printf "__VGPU_DMESG_%s__\n" "$status"} 4
  expect_status "__VGPU_DMESG_OK__" "__VGPU_DMESG_BAD__" 4

  send_cmd {if test -f /root/local-env.sh; then status=OK; else status=MISSING; fi; printf "__LOCALENV_%s__\n" "$status"} 5
  expect_status "__LOCALENV_OK__" "__LOCALENV_MISSING__" 5

  send_cmd {. /root/local-env.sh >/dev/null 2>&1; if test $? -eq 0; then status=OK; else status=FAIL; fi; export DISPLAY=:0; printf "__LOCALENV_SRC_%s__\n" "$status"} 5
  expect_status "__LOCALENV_SRC_OK__" "__LOCALENV_SRC_FAIL__" 5

  send_cmd {ls -l /usr/lib/dri 2>/dev/null || true; if test -f /etc/semu-test-tools-virgl && test -e /usr/lib/dri/virtio_gpu_dri.so; then status=OK; else status=MISSING; fi; printf "__VIRGL_GUEST_DRIVER_%s__\n" "$status"} 5
  expect_status "__VIRGL_GUEST_DRIVER_OK__" "__VIRGL_GUEST_DRIVER_MISSING__" 5

  send_cmd {if command -v glxinfo >/dev/null 2>&1 && command -v glxgears >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf "__VIRGL_APPS_%s__\n" "$status"} 5
  expect_status "__VIRGL_APPS_OK__" "__VIRGL_APPS_MISSING__" 5

  send_cmd {if test -S /tmp/.X11-unix/X0; then status=RUNNING; elif command -v Xorg >/dev/null 2>&1; then rm -f /tmp/.X0-lock; Xorg :0 -noreset -nolisten tcp >/tmp/xorg.log 2>&1 & echo $! >/tmp/xorg.pid; status=STARTED; else status=MISSING; fi; printf "__VIRGL_XORG_%s__\n" "$status"} 6
  expect {
    -exact "__VIRGL_XORG_RUNNING__" {}
    -exact "__VIRGL_XORG_STARTED__" {}
    -exact "__VIRGL_XORG_MISSING__" { exit 6 }
    timeout { exit 6 }
  }

  set xorg_cmd "i=0; status=FAIL; while test \$i -lt $env(VGPU3D_XORG_RETRIES); do if test -S /tmp/.X11-unix/X0; then status=READY; break; fi; sleep $env(VGPU3D_XORG_SLEEP); i=\$((i + 1)); done; printf \"__VIRGL_XORG_%s__\\n\" \"\$status\""
  send_cmd $xorg_cmd 6
  expect {
    -exact "__VIRGL_XORG_READY__" {}
    -exact "__VIRGL_XORG_FAIL__" {
      send -- "cat /tmp/xorg.log 2>/dev/null || true\r"
      exit 6
    }
    timeout { exit 6 }
  }

  set glxinfo_cmd "rm -f /tmp/vgpu3d-glxinfo.log; i=0; status=FAIL; while test \$i -lt $env(VGPU3D_GLXINFO_RETRIES); do DISPLAY=:0 glxinfo -B >/tmp/vgpu3d-glxinfo.log 2>&1 && { status=OK; break; }; sleep $env(VGPU3D_GLXINFO_SLEEP); i=\$((i + 1)); done; head -80 /tmp/vgpu3d-glxinfo.log; if test \"\$status\" = OK && grep -Eiq 'OpenGL renderer string:.*virgl|Device:.*virgl|virgl' /tmp/vgpu3d-glxinfo.log; then result=OK; else result=BAD; echo '--- forced virgl loader diagnostic ---'; DISPLAY=:0 LIBGL_DEBUG=verbose MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu glxinfo -B >/tmp/vgpu3d-glxinfo-virtio.log 2>&1 || true; head -120 /tmp/vgpu3d-glxinfo-virtio.log; echo '--- xorg log tail ---'; tail -120 /tmp/xorg.log 2>/dev/null || true; echo '--- dmesg virgl tail ---'; dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' | tail -120 || true; fi; printf \"__VGPU_GLXINFO_%s__\\n\" \"\$result\""
  send_cmd $glxinfo_cmd 7
  expect_status "__VGPU_GLXINFO_OK__" "__VGPU_GLXINFO_BAD__" 7

  set glxgears_cmd "rm -f /tmp/vgpu3d-glxgears.log; if command -v timeout >/dev/null 2>&1; then DISPLAY=:0 timeout $env(VGPU3D_GLXGEARS_SECONDS)s glxgears >/tmp/vgpu3d-glxgears.log 2>&1; rc=\$?; else DISPLAY=:0 glxgears >/tmp/vgpu3d-glxgears.log 2>&1 & pid=\$!; sleep $env(VGPU3D_GLXGEARS_SECONDS); kill \$pid 2>/dev/null || true; wait \$pid 2>/dev/null || true; rc=124; fi; head -40 /tmp/vgpu3d-glxgears.log; if { test \"\$rc\" -eq 0 || test \"\$rc\" -eq 124; } && grep -Eiq 'Running synchronized|frames in|GL_RENDERER|virgl' /tmp/vgpu3d-glxgears.log; then result=OK; else result=BAD; echo '--- xorg log tail ---'; tail -120 /tmp/xorg.log 2>/dev/null || true; echo '--- dmesg virgl tail ---'; dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' | tail -120 || true; fi; printf \"__VGPU_GLXGEARS_%s__\\n\" \"\$result\""
  send_cmd $glxgears_cmd 8
  expect_status "__VGPU_GLXGEARS_OK__" "__VGPU_GLXGEARS_BAD__" 8
}

set timeout $env(TIMEOUT)
spawn make check

login_guest 1
run_compact_probe "initial"

for {set run 1} {$run <= $env(VGPU3D_REBOOT_RUNS)} {incr run} {
  puts "--- vgpu 3D reboot cycle $run/$env(VGPU3D_REBOOT_RUNS) ---"
  expect "# " { send "sync; reboot -f\r" } timeout { exit 9 }
  login_guest 10
  run_compact_probe "after reboot $run"
}
DONE

ret="$?"
set -e

MESSAGES=(
  "PASS: visible vgpu 3D reboot stress"
  "FAIL: initial boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: virtio-gpu basic checks failed (/dev/dri/card0, /dev/dri/renderD128, or virtio_gpu binding)"
  "FAIL: virgl/capset/resource_blob/host_visible dmesg checks failed"
  "FAIL: guest VirGL tools/driver missing or local-env.sh failed"
  "FAIL: guest Xorg did not start on :0"
  "FAIL: glxinfo -B failed or did not report a virgl renderer"
  "FAIL: glxgears did not start cleanly"
  "FAIL: guest reboot command was not sent"
  "FAIL: reboot/login prompt not found after guest reboot"
)

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
