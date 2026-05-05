#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    print_error "FAIL: visible VirGL smoke needs host DISPLAY or WAYLAND_DISPLAY"
    exit 1
fi

cleanup
trap cleanup EXIT

make ENABLE_VIRGL=1 semu minimal.dtb Image test-tools.img

set +e
expect <<'DONE'
set timeout $env(TIMEOUT)
spawn ./semu -k Image -c 1 -b minimal.dtb -d test-tools.img

expect "buildroot login:" { send "root\r" } timeout { exit 1 }
expect "# "              { send "uname -a\r" } timeout { exit 2 }
expect "riscv32 GNU/Linux" {}

expect "# " {
  send "if test -c /dev/dri/card0 && test -c /dev/dri/renderD128; then status=OK; else status=MISSING; fi; printf \"__VIRGL_DRM_%s__\\n\" \"\$status\"\r"
} timeout { exit 3 }
expect {
  -exact "__VIRGL_DRM_OK__" {}
  -exact "__VIRGL_DRM_MISSING__" { exit 3 }
  timeout { exit 3 }
}

expect "# " {
  send "if command -v glxinfo >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf \"__GLXINFO_%s__\\n\" \"\$status\"\r"
} timeout { exit 4 }
expect {
  -exact "__GLXINFO_OK__" {}
  -exact "__GLXINFO_MISSING__" { exit 4 }
  timeout { exit 4 }
}

expect "# " {
  send "DISPLAY=:0 glxinfo -B >/tmp/glxinfo.log 2>&1; rc=\$?; head -80 /tmp/glxinfo.log; if test \"\$rc\" -eq 0 && grep -Eiq 'virgl|OpenGL renderer string' /tmp/glxinfo.log && grep -Eiq 'virgl' /tmp/glxinfo.log; then status=OK; else status=FAIL; fi; printf \"__VIRGL_RENDERER_%s__\\n\" \"\$status\"\r"
} timeout { exit 5 }
expect {
  -exact "__VIRGL_RENDERER_OK__" {}
  -exact "__VIRGL_RENDERER_FAIL__" { exit 5 }
  timeout { exit 5 }
}

expect "# " {
  send "if command -v glxgears >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf \"__GLXGEARS_%s__\\n\" \"\$status\"\r"
} timeout { exit 6 }
expect {
  -exact "__GLXGEARS_OK__" {}
  -exact "__GLXGEARS_MISSING__" { exit 6 }
  timeout { exit 6 }
}

expect "# " {
  send "if command -v timeout >/dev/null 2>&1; then DISPLAY=:0 timeout 5s glxgears >/tmp/glxgears.log 2>&1; rc=\$?; else DISPLAY=:0 glxgears >/tmp/glxgears.log 2>&1 & pid=\$!; sleep 5; kill \$pid 2>/dev/null; rc=124; fi; head -40 /tmp/glxgears.log; if test \"\$rc\" -eq 0 || test \"\$rc\" -eq 124; then status=OK; else status=FAIL; fi; printf \"__VIRGL_GEARS_%s__\\n\" \"\$status\"\r"
} timeout { exit 7 }
expect {
  -exact "__VIRGL_GEARS_OK__" {}
  -exact "__VIRGL_GEARS_FAIL__" { exit 7 }
  timeout { exit 7 }
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
  "FAIL: DRM card/render node missing"
  "FAIL: glxinfo missing from guest image"
  "FAIL: glxinfo did not report VirGL renderer"
  "FAIL: glxgears missing from guest image"
  "FAIL: glxgears did not start"
)

if [ "${ret}" -eq 0 ]; then
    print_success "${MESSAGES[0]}"
else
    print_error "${MESSAGES[${ret}]:-FAIL: unknown VirGL smoke error (${ret})}"
fi

exit "${ret}"
