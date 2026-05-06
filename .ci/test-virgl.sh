#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    print_error "FAIL: visible VirGL smoke needs host DISPLAY or WAYLAND_DISPLAY"
    exit 1
fi

if ! command -v sdl2-config >/dev/null 2>&1; then
    print_error "FAIL: visible VirGL smoke needs sdl2-config in PATH"
    exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1 ||
   ! pkg-config --exists virglrenderer epoxy gl egl; then
    print_error "FAIL: visible VirGL smoke needs pkg-config packages: virglrenderer epoxy gl egl"
    exit 1
fi

SEMU_VIRGL_REBOOT_TEST="${SEMU_VIRGL_REBOOT_TEST:-0}"
export SEMU_VIRGL_REBOOT_TEST

cleanup
trap cleanup EXIT

make -B ENABLE_VIRGL=1 semu minimal.dtb
make Image test-tools.img

set +e
expect <<'DONE'
set timeout $env(TIMEOUT)
spawn ./semu -k Image -c 1 -b minimal.dtb -d test-tools.img

expect "buildroot login:" { send "root\r" } timeout { exit 1 }
expect "# "              { send "uname -a\r" } timeout { exit 2 }
expect "riscv32 GNU/Linux" {}

expect "# " {
  send "if test -f /root/local-env.sh; then . /root/local-env.sh; fi; printf \"__LOCALENV_DONE__\\n\"\r"
} timeout { exit 3 }
expect {
  -exact "__LOCALENV_DONE__" {}
  timeout { exit 3 }
}

expect "# " {
  send "ls -l /usr/lib/dri 2>/dev/null || true; if test -f /etc/semu-test-tools-virgl && test -e /usr/lib/dri/virtio_gpu_dri.so; then status=OK; else status=MISSING; fi; printf \"__VIRGL_GUEST_DRIVER_%s__\\n\" \"\$status\"\r"
} timeout { exit 4 }
expect {
  -exact "__VIRGL_GUEST_DRIVER_OK__" {}
  -exact "__VIRGL_GUEST_DRIVER_MISSING__" { exit 4 }
  timeout { exit 4 }
}

expect "# " {
  send "if test -c /dev/dri/card0 && test -c /dev/dri/renderD128; then status=OK; else status=MISSING; fi; printf \"__VIRGL_DRM_%s__\\n\" \"\$status\"\r"
} timeout { exit 5 }
expect {
  -exact "__VIRGL_DRM_OK__" {}
  -exact "__VIRGL_DRM_MISSING__" { exit 5 }
  timeout { exit 5 }
}

expect "# " {
  send "if command -v glxinfo >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf \"__GLXINFO_%s__\\n\" \"\$status\"\r"
} timeout { exit 6 }
expect {
  -exact "__GLXINFO_OK__" {}
  -exact "__GLXINFO_MISSING__" { exit 6 }
  timeout { exit 6 }
}

expect "# " {
  send "if test -S /tmp/.X11-unix/X0; then status=RUNNING; elif command -v Xorg >/dev/null 2>&1; then rm -f /tmp/.X0-lock; Xorg :0 -noreset -nolisten tcp >/tmp/xorg.log 2>&1 & echo \$! >/tmp/xorg.pid; status=STARTED; else status=MISSING; fi; printf \"__VIRGL_XORG_%s__\\n\" \"\$status\"\r"
} timeout { exit 7 }
expect {
  -exact "__VIRGL_XORG_RUNNING__" {}
  -exact "__VIRGL_XORG_STARTED__" {}
  -exact "__VIRGL_XORG_MISSING__" { exit 7 }
  timeout { exit 7 }
}

expect "# " {
  send "i=0; status=FAIL; while test \$i -lt 20; do if test -S /tmp/.X11-unix/X0; then status=READY; break; fi; sleep 1; i=\$((i + 1)); done; printf \"__VIRGL_XORG_%s__\\n\" \"\$status\"\r"
} timeout { exit 7 }
expect {
  -exact "__VIRGL_XORG_READY__" {}
  -exact "__VIRGL_XORG_FAIL__" {
    send "cat /tmp/xorg.log 2>/dev/null || true\r"
    exit 7
  }
  timeout { exit 7 }
}

expect "# " {
  send "DISPLAY=:0 glxinfo -B >/tmp/glxinfo.log 2>&1; rc=\$?; head -80 /tmp/glxinfo.log; if test \"\$rc\" -eq 0 && grep -Eiq 'OpenGL renderer string:.*virgl|virgl' /tmp/glxinfo.log; then status=OK; else status=FAIL; echo '--- forced virgl loader diagnostic ---'; DISPLAY=:0 LIBGL_DEBUG=verbose MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu glxinfo -B >/tmp/glxinfo-virtio.log 2>&1 || true; head -120 /tmp/glxinfo-virtio.log; echo '--- xorg log tail ---'; tail -120 /tmp/xorg.log 2>/dev/null || true; fi; printf \"__VIRGL_RENDERER_%s__\\n\" \"\$status\"\r"
} timeout { exit 8 }
expect {
  -exact "__VIRGL_RENDERER_OK__" {}
  -exact "__VIRGL_RENDERER_FAIL__" {
    expect "# " {
      send "echo '--- kernel virtio-gpu diagnostic ---'; dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|cap sets|features:|virgl' | tail -n 120 || true\r"
    } timeout { exit 8 }
    expect "# " {
      send "echo '--- drm debugfs diagnostic ---'; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; for f in /sys/kernel/debug/dri/*/virtio-gpu-features; do test -f \$f && { echo ---\$f---; cat \$f; }; done\r"
    } timeout { exit 8 }
    expect "# " {
      send "echo '--- drm sysfs diagnostic ---'; for n in /sys/class/drm/card0 /sys/class/drm/renderD128; do echo ---\$n---; ls -l \$n/device/driver 2>/dev/null || true; cat \$n/device/uevent 2>/dev/null || true; done\r"
    } timeout { exit 8 }
    expect "# " {
      send "echo '--- xorg dri diagnostic ---'; grep -Ei 'dri|glamor|modeset|virtio|swrast|render' /var/log/Xorg.0.log /tmp/xorg.log 2>/dev/null | tail -n 160 || true\r"
    } timeout { exit 8 }
    exit 8
  }
  timeout { exit 8 }
}

expect "# " {
  send "if command -v glxgears >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf \"__GLXGEARS_%s__\\n\" \"\$status\"\r"
} timeout { exit 9 }
expect {
  -exact "__GLXGEARS_OK__" {}
  -exact "__GLXGEARS_MISSING__" { exit 9 }
  timeout { exit 9 }
}

expect "# " {
  send "if command -v timeout >/dev/null 2>&1; then DISPLAY=:0 timeout 5s glxgears >/tmp/glxgears.log 2>&1; rc=\$?; else DISPLAY=:0 glxgears >/tmp/glxgears.log 2>&1 & pid=\$!; sleep 5; kill \$pid 2>/dev/null; rc=124; fi; head -40 /tmp/glxgears.log; if test \"\$rc\" -eq 0 || test \"\$rc\" -eq 124; then status=OK; else status=FAIL; fi; printf \"__VIRGL_GEARS_%s__\\n\" \"\$status\"\r"
} timeout { exit 10 }
expect {
  -exact "__VIRGL_GEARS_OK__" {}
  -exact "__VIRGL_GEARS_FAIL__" { exit 10 }
  timeout { exit 10 }
}

if {$env(SEMU_VIRGL_REBOOT_TEST) eq "1"} {
  expect "# " {
    send "reboot -f\r"
  } timeout { exit 11 }
  expect "buildroot login:" { send "root\r" } timeout { exit 11 }
  expect "# "              { send "uname -a\r" } timeout { exit 11 }
  expect "riscv32 GNU/Linux" {}
}

send "\x01"
send "x"
exit 0
DONE

ret="$?"
set -e

MESSAGES=(
  "PASS: visible VirGL virtio-gpu smoke checks"
  "FAIL: boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: guest local environment setup did not complete"
  "FAIL: guest Mesa VirGL driver missing; rebuild test-tools.img with scripts/build-image.sh --virgl"
  "FAIL: DRM card/render node missing"
  "FAIL: glxinfo missing from guest image"
  "FAIL: guest Xorg did not start on :0"
  "FAIL: glxinfo did not report VirGL renderer"
  "FAIL: glxgears missing from guest image"
  "FAIL: glxgears did not start"
  "FAIL: guest reboot/reset check failed"
)

if [ "${ret}" -eq 0 ]; then
    print_success "${MESSAGES[0]}"
    printf 'NOTE: glxgears printed the first 40 lines of /tmp/glxgears.log; compare FPS while idle and while moving the mouse quickly during manual smoke.\n'
else
    print_error "${MESSAGES[${ret}]:-FAIL: unknown VirGL smoke error (${ret})}"
fi

exit "${ret}"
