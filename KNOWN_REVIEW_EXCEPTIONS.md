# Known Review Exception: debug-mode shutdown hang with VIRTIOINPUT

## Summary

There is a known issue where `semu` can hang during shutdown when running with both `-g` and `VIRTIOINPUT` enabled and the SDL window is closed during debug mode.

This issue is currently accepted and should not be re-reported in routine code review unless a patch directly changes the affected debugger/shutdown path, expands the issue beyond debug mode, or materially changes its severity or scope.

## Affected configuration

- `-g` enabled
- `VIRTIOINPUT` enabled
- SDL window backend active

## Affected path

- main thread: `window_main_loop()`
- emulator thread: `semu_run_debug()`
- debugger path: `gdbstub_init()` and `gdbstub_run()`

## Observable scenarios

### Scenario 1: window closed before GDB connects

If the user closes the SDL window before any GDB client connects, the emulator thread may still be blocked in `accept()` inside `conn_init()`, which is called from `gdbstub_init()`.

Sequence:

1. Run `semu -g`
2. SDL window opens
3. No GDB client has connected yet
4. User closes the SDL window
5. Main thread returns from `window_main_loop()`
6. Main thread blocks in `pthread_join(emu_thread, ...)`
7. Emulator thread remains blocked in `accept()`
8. Process does not exit

### Scenario 2: GDB connected, stub idle waiting for next packet

After GDB connects, `gdbstub_run()` waits for the next remote packet through `conn_recv_packet()`, which blocks in `poll(-1)`.

Sequence:

1. GDB connects successfully
2. Stub becomes idle waiting for the next packet
3. User closes the SDL window
4. Main thread returns from `window_main_loop()`
5. Main thread blocks in `pthread_join(emu_thread, ...)`
6. Emulator thread remains blocked in `poll(-1)` inside `conn_recv_packet()`
7. Process does not exit until the GDB connection changes state, such as disconnecting

## Why the current shutdown path does not solve it

The current debug shutdown path only works after execution reaches `semu_cont()`. In that path, `semu_cont()` checks `window_is_closed()` and returns `ACT_SHUTDOWN`, allowing `gdbstub_run()` to exit cleanly.

That does not help in the two cases above, because the emulator thread is blocked earlier:

- in `accept()` during `gdbstub_init()`
- in `poll(-1)` inside `conn_recv_packet()`

In both cases, the thread never reaches `semu_cont()`, so the window-close state is never observed there.

The async interrupt thread in `mini-gdbstub` does not solve this either, because it is only relevant while handling `EVENT_CONT`, not while blocked in connection setup or idle packet wait.

## Root cause

Closing the SDL window currently has no way to interrupt blocking waits inside `mini-gdbstub`.

There is no public API to interrupt:

- `accept()` in `conn_init()`
- `poll(-1)` in `conn_recv_packet()`

As a result, the main thread can request shutdown, but the emulator thread may remain blocked indefinitely inside the debugger transport layer.

## Possible fixes

Potential proper fixes inside `mini-gdbstub` include:

- adding an explicit interrupt API such as `gdbstub_interrupt()`
- changing `conn_recv_packet()` to wait on both the GDB socket and an interrupt fd
- making the blocking `accept()` path interruptible through the same mechanism

A possible non-submodule workaround would be sending a signal such as `SIGUSR1` to the emulator thread so that `accept()` / `poll()` return with `EINTR`, together with explicit handling in `gdbstub_init()` and `gdbstub_run()`.

## Current workaround

- No workaround for Scenario 1
- For Scenario 2, if GDB is already connected, issuing `continue` allows execution to enter `semu_cont()`, which can then observe `window_is_closed()` and return `ACT_SHUTDOWN`

## Review guidance

Do not report this as a new finding in normal review unless:

- the patch directly modifies the debug shutdown path
- the patch touches `semu_run_debug()`, `window_main_loop()`, `gdbstub_init()`, `gdbstub_run()`, `conn_init()`, or `conn_recv_packet()`
- the issue is no longer debug-only
- the patch worsens the behavior or changes the user-visible impact

# issue 2

問題摘要
在 vinput-pr 裡，window backend 接進 emulator thread 之後，SMP/threaded window 路徑有一個 exit-status 問題：如果 emulator 已經遇到 fatal error，但使用者同時把視窗關掉，最後 process 可能回傳 success (0)，把真正的失敗蓋掉。

問題現象
可能會看到 vm error ... 已經印出來，但整個 semu 還是以正常結束退出。對 CI、script、或外層 harness 來說，這會被誤判成 pass。

目前程式行為
在 main.c (line 235)，window_is_closed() 會直接把 emu->stopped 設成 true。
在 main.c (line 1047)，hart fatal error 也會走 vm_error_report(hart) 然後把 emu->stopped 設成 true。
但 cleanup 時在 main.c (line 1490) 只用 window_is_closed() 來判斷是否算錯誤，所以只要視窗已經關掉，就可能不回非零 exit code。

根本原因
emu->stopped 只表示「要停了」，沒有保存「為什麼停」。
vinput PR 把 frontend/window close 這條 shutdown 路徑接進 emulator thread 後，window close 和 fatal error 共用同一個 stop flag；cleanup 又是事後用 window_is_closed() 反推 stop cause，導致 window close 可能覆寫 fatal error 的語義。

範圍判斷
這不是 master 原本就有的問題，是 vinput-pr 引入的行為變化。
它主要影響的是 vinput PR 帶進來的 threaded window/SMP shutdown 路徑，不是 virtio-input device spec 本身的問題。

建議修法方向
之後若要修，應該保留 stop cause，而不是只看 emu->stopped 或事後再查 window_is_closed()。原則上 fatal error 的優先級應該高於 user close。

---

# Issue 3: virtio-fs `num_request_queues` off-by-one 導致 SMP=4 kernel panic

## 問題摘要

`virtio-fs` 裝置宣告的 `num_request_queues` 值與 host 實際的 queue slot 數量不一致，導致 guest driver probe 時存取到無效的 queue index。這使 guest `virtio_fs_probe()` fail、觸發 double-free，最終因 SLUB freelist corruption 產生 kernel panic。

此問題在 `SMP=4` + `vinput-pr` 分支上必現，因為 `virtio-input` device 的後續 allocation 會立刻消費到被 corrupt 的 freelist pointer。

## 重現方式

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

穩定在 ~0.94s emulated time 產生 panic:

```text
[    0.938126] Unable to handle kernel paging request at virtual address 72697636
[    0.954861] [<c00f68a0>] __kmalloc_cache_noprof+0x9c/0x128
[    0.956018] [<c026f490>] vm_find_vqs+0x188/0x408
[    0.957207] [<c0270168>] virtinput_init_vqs+0x70/0x98
[    0.958406] [<c027026c>] virtinput_probe+0x70/0x528
```

## 根本原因

### Host 端宣告與實際 queue 數量不一致

`virtio-fs.c:938` (修正前):
```c
PRIV(vfs)->num_request_queues = 3;
```

`device.h:459`:
```c
virtio_fs_queue_t queues[3];   /* 只有 index 0, 1, 2 */
```

根據 virtio-fs spec，guest 看到 `num_request_queues = 3` 後會建立:
- 1 個 hiprio queue (index 0)
- 3 個 request queue (index 1, 2, 3)
- 共計 4 個 queue

但 host 的 `queues[3]` 只有 3 個 slot (index 0-2)，所以 `QueueSel=3` 會被 host 判為 invalid。

### 完整因果鏈

1. Guest 寫入 `QueueSel=3`，host 判定 invalid，**不更新內部 `QueueSel`**，仍停在前一個值 2
2. Guest 接著讀 `QueueReady`，實際讀到的是 **queue 2 的 `ready=1`**（因為 queue 2 剛設完）
3. Guest `virtio_mmio.c` 碰到 `readl(QUEUE_READY) != 0`，回傳 `-ENOENT`
4. Guest `virtio_fs_probe()` 因此 fail
5. Guest `virtio_fs_setup_vqs()` 的 error path free 了 `mq_map` / `vqs`
6. Guest `virtio_fs_probe()` 接著呼叫 `kobject_put(&fs->kobj)` → `virtio_fs_ktype_release()` **再 free 一次**
7. `mq_map` 被 double-free，SLUB freelist 形成 self-link
8. 後續 `kstrdup_const()` 重用該 object 作 16-byte name buffer，寫入 `.vir...` 字串
9. SLUB 把該字串內容當 freelist pointer 消費，得到 `0x7269762e` (ASCII `.vir`) / `0x72697636` (ASCII `6vir`)
10. `virtio_input` probe 做 `kmalloc` 時踩到這個非法 pointer → **kernel panic**

## 修正方式

把 `num_request_queues` 從 `3` 改成 `2`，使 guest 建立 `1 + 2 = 3` 個 queue，匹配 host 的 `queues[3]`:

`virtio-fs.c:938`:
```c
/* 修正前 */
PRIV(vfs)->num_request_queues = 3;

/* 修正後 */
PRIV(vfs)->num_request_queues = 2;
```

### 驗證結果

| 測試條件 | `num_request_queues` | 結果 |
|----------|---------------------|------|
| `SMP=4`, 修正前 | `3` | Kernel panic at ~0.94s |
| `SMP=4`, 修正後 | `2` | VirtIO Keyboard/Mouse 成功 probe，無 panic |

---

# Issue 4: PLIC context 解析錯誤 + SBI IPI/RFENCE 越界存取

## 問題摘要

`vinput-pr` 分支在合入時帶進了兩組獨立的 correctness bug:

1. **PLIC `plic_reg_read` / `plic_reg_write` 的 context 計算使用了錯誤的 bit-trick**，導致 hart 2/3 永遠無法正確讀寫 enable register 和 claim/complete register
2. **SBI IPI 和 RFENCE handler 的迴圈沒有做上界檢查**，可能寫入越界的 `ssip[]` 或存取越界的 `hart[]`

這兩個 bug 本身不是 Issue 3 kernel panic 的直接根因（修掉它們不影響 panic 的重現），但都是真正的 correctness issue，在更複雜的 workload 下可能導致錯誤行為。

## Bug 4a: PLIC context 計算錯誤

### 問題

原始的 `plic_reg_read()` / `plic_reg_write()` 使用一段 bit-manipulation 來解析 MMIO address 到 context index:

`plic.c:35-37` (修正前):
```c
int addr_mask = MASK(ilog2(addr)) ^ (1 & addr);
int context = (addr_mask & addr);
context >>= __builtin_ffs(context) - (__builtin_ffs(context) & 1);
```

這段邏輯對 context 0 和 1 恰好能算出正確結果，但對 context 2 以上會算錯。

根據 PLIC spec，MMIO register layout 是:

| 區域 | 起始 word offset | stride (words) | 用途 |
|------|-----------------|----------------|------|
| Enable | `0x800` | `0x20` | per-context interrupt enable |
| Threshold | `0x80000` | `0x400` | per-context priority threshold |
| Claim/Complete | `0x80001` | `0x400` | per-context claim & complete |

正確的 context 計算應該是簡單的除法:

- Enable 區: `context = (addr - 0x800) / 0x20`
- Context 區: `context = (addr - 0x80000) / 0x400`

### 影響

在 `SMP=4` 下，hart 2 和 hart 3 寫入 enable register 時，context 被算錯 → `plic->ie[context]` 寫到錯誤的 slot → `plic_update_interrupts()` 在比對 `plic->ip & plic->ie[i]` 時，hart 2/3 的 enable 永遠是 0 → **hart 2/3 永遠收不到外部中斷**。

### 修正方式

用直觀的範圍檢查 + 除法取代 bit-trick:

`plic.c` `plic_reg_read()` (修正後):
```c
/* Enable registers: word 0x800 + context * 0x20 (byte 0x2000, stride
 * 0x80). Only the first word per context is meaningful for <= 32 sources.
 */
if (addr >= 0x800 && addr < 0xC00) {
    int context = (addr - 0x800) / 0x20;
    if ((addr - 0x800) % 0x20 == 0)
        *value = plic->ie[context];
    return true;
}

/* Context registers: word 0x80000 + context * 0x400 (byte 0x200000,
 * stride 0x1000).  Offset 0 = threshold, offset 1 = claim/complete.
 */
if (addr >= 0x80000 && addr < 0x88000) {
    int context = (addr - 0x80000) / 0x400;
    int offset = (addr - 0x80000) % 0x400;
    if (offset == 0) {
        *value = 0;
        return true;
    }
    if (offset == 1) {
        /* claim */
        *value = 0;
        uint32_t candidates = plic->ip & plic->ie[context];
        if (candidates) {
            *value = ilog2(candidates);
            plic->ip &= ~(1 << (*value));
        }
        return true;
    }
    return true;
}
```

`plic_reg_write()` 同理改為範圍檢查 + 除法。

## Bug 4b: SBI IPI / RFENCE 迴圈越界

### 問題

原始的 IPI 和 RFENCE handler 有三個問題:

#### (1) `hart_mask_base == -1` 的比較永遠失敗

`main.c` (修正前):
```c
uint64_t hart_mask, hart_mask_base;
/* ... */
hart_mask_base = (uint64_t) hart->x_regs[RV_R_A1];
if (hart_mask_base == 0xFFFFFFFFFFFFFFFF) {
```

`hart->x_regs[RV_R_A1]` 是 `uint32_t`，值為 `0xFFFFFFFF`。cast 成 `uint64_t` 後變成 `0x00000000FFFFFFFF`，永遠不等於 `0xFFFFFFFFFFFFFFFF`。因此 "broadcast to all harts" 的分支永遠走不到。

#### (2) 迴圈沒有上界檢查

`main.c` (修正前):
```c
for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++)
    data->sswi.ssip[i] = hart_mask & 1;
```

迴圈變數 `i` 沒有 `i < n_hart` 的上界，如果 `hart_mask` 的高位有 set bit，`i` 會超過 `ssip[]` 的 array bound。

#### (3) IPI 無條件寫入 `ssip[i]`

即使 `hart_mask & 1 == 0`，原始程式碼也會把 `ssip[i]` 寫成 `0`，可能清掉其他 hart 之前送出的、還沒被消費的 pending IPI。

RFENCE handler 也有同樣的 `uint64_t` 比較失敗和迴圈越界問題。

### 修正方式

`main.c` `handle_sbi_ecall_IPI()` (修正後):
```c
uint32_t hart_mask, hart_mask_base;
/* ... */
hart_mask = hart->x_regs[RV_R_A0];
hart_mask_base = hart->x_regs[RV_R_A1];
if (hart_mask_base == (uint32_t) -1) {
    for (uint32_t i = 0; i < hart->vm->n_hart; i++)
        data->sswi.ssip[i] = 1;
} else {
    for (uint32_t i = hart_mask_base;
         hart_mask && i < hart->vm->n_hart; hart_mask >>= 1, i++) {
        if (hart_mask & 1)
            data->sswi.ssip[i] = 1;
    }
}
```

`handle_sbi_ecall_RFENCE()` 也做同樣修正:
```c
uint32_t hart_mask, hart_mask_base;
/* ... */
hart_mask = hart->x_regs[RV_R_A0];
hart_mask_base = hart->x_regs[RV_R_A1];
if (hart_mask_base == (uint32_t) -1) {
    for (uint32_t i = 0; i < hart->vm->n_hart; i++)
        mmu_invalidate_range(hart->vm->hart[i], start_addr, size);
} else {
    for (uint32_t i = hart_mask_base;
         hart_mask && i < hart->vm->n_hart; hart_mask >>= 1, i++) {
        if (hart_mask & 1)
            mmu_invalidate_range(hart->vm->hart[i], start_addr, size);
    }
}
```

修正重點:
- `uint64_t` → `uint32_t`：使 `== (uint32_t) -1` 比較正確
- 迴圈加上 `i < hart->vm->n_hart` 上界
- IPI 只在 `hart_mask & 1` 時 set `ssip[i] = 1`，不無條件覆寫
