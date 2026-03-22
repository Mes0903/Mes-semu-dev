# `vinput-pr` / `SMP=4` kernel panic 調查紀錄

日期: 2026-03-22  
分支: `vinput-pr`  
調查目標: 找出為什麼在 `vinput-pr` 上執行 `make check ENABLE_INPUT_DEBUG=1 SMP=4` 時，guest 會在早期 boot 階段 kernel panic，而 `master` 不會。

## 目前結論摘要

目前最合理的結論是:

- `vinput-pr` 新增的兩個 `virtio,mmio` input device node 會把 Linux 早期 boot 的 driver-core / devtmpfs / slab / percpu 快路徑壓出來。
- 真正被引爆的問題不像是 `virtio-input` event path 或 PLIC IRQ 編號，而是 semu 現有的 `SMP=4` 語意缺口。
- 這個缺口目前最像落在 `local interrupt masking + percpu fastpath` 這一層，`aq/rl` / `FENCE` 沒完整實作則是另一個同樣存在的 correctness gap，可能有加重風險，但目前不能只靠證據把這次 crash 100% 指到它。
- `virtio-input` 在這件事裡更像 trigger，不像 direct corrupter。

## 相關 code 位置

- `virtio-input` 裝置節點: [`minimal.dts`](../minimal.dts) 第 91-103 行
- SBI IPI / RFENCE handler: [`main.c`](../main.c) 第 492-556 行
- RV32 AMO path: [`riscv.c`](../riscv.c) 第 980-1020 行
- `FENCE/FENCE.I` 目前是 no-op: [`riscv.c`](../riscv.c) 第 1112-1117 行

## 關鍵觀察

1. panic stack 很穩定地落在 guest:
   - `virtinput_init_vqs`
   - `vm_find_vqs`
   - `__kmalloc_cache_noprof`
2. bad pointer 幾乎固定長這樣:
   - `0x7269762e`
   - `0x72697636`
3. 這兩個值不像合法 kernel pointer，比較像 stale object content。
4. 把 `virtio-input` node 從 DT 拿掉後，系統可以越過原本 panic 點。
5. 把 node 改成 dummy compatible 後，也可以越過原本 panic 點。
6. 把 node 留著、但拿掉 `interrupts` 後，雖然 probe 不再正常 request IRQ，後面仍然會用同一組壞 pointer 爆掉。

## 詳細調查過程

### 1. 直接重現 panic

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.922787] virtiofs virtio4: probe with driver virtiofs failed with error -2
[    0.938126] Unable to handle kernel paging request at virtual address 72697636
[    0.954861] [<c00f68a0>] __kmalloc_cache_noprof+0x9c/0x128
[    0.956018] [<c026f490>] vm_find_vqs+0x188/0x408
[    0.957207] [<c0270168>] virtinput_init_vqs+0x70/0x98
[    0.958406] [<c027026c>] virtinput_probe+0x70/0x528
```

判斷:

- crash 發生在 guest `virtio-input` driver probe 建立 virtqueue 的階段。
- 這時還沒進到真正的 host input event delivery，因此一開始就不太像 `window-events` / event injection race。

---

### 2. 和 `master` 對照

這一步是使用者另外在 `master` 分支上做的對照。結果是:

- 同樣 kernel
- 同樣 `SMP=4`
- `master` 能正常 boot

關鍵輸出片段:

```text
[    3.915110] virtio_blk virtio1: 1/0/0 default/read/poll queues
[    3.925236] virtio_blk virtio1: [vda] 2097152 512-byte logical blocks
[    3.948839] virtio_net virtio0: Assigned random MAC address
[    4.399019] Run /init as init process
[*] Attempting to mount /dev/vda
```

判斷:

- 問題不是「semu 只要 `SMP=4` 就一定不行」。
- 是 `vinput-pr` 這條線新增的東西，讓 latent SMP bug 被穩定炸出來。

---

### 3. 暫時拿掉 input node 的 `interrupts`

實驗前的暫時修改:

- 在 `minimal.dts` 的 `keyboard0` / `mouse0` 節點上移除 `interrupts = <7>` / `<8>`

執行指令:

```bash
make -j4 semu minimal.dtb
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
virtio-mmio f4900000.virtio: error -ENXIO: IRQ index 0 not found
virtio-mmio f5000000.virtio: error -ENXIO: IRQ index 0 not found
```

之後又在較後面的位置炸掉，當時仍然看見相同類型的壞 pointer:

```text
s1 : 7269762e
a5 : 72697636
```

判斷:

- 問題不是「只有正常 request IRQ / 綁上 handler 後才會發生」。
- 也就是說，單純改 PLIC 或 IRQ 編號，解掉這次 panic 的機率很低。

---

### 4. 暫時把 input node 整個從 DT 拿掉

實驗前的暫時修改:

- 在 `minimal.dts` 暫時移除整段 `SEMU_FEATURE_VIRTIOINPUT` 節點

執行指令:

```bash
make -j4 semu minimal.dtb
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    3.080367] Serial: 8250/16550 driver, 4 ports, IRQ sharing disabled
[    3.915110] virtio_blk virtio1: 1/0/0 default/read/poll queues
[    3.948839] virtio_net virtio0: Assigned random MAC address
[    4.399019] Run /init as init process
[*] Attempting to mount /dev/vda
```

判斷:

- 這個對照很強，因為它把 `VIRTIOINPUT` 編譯選項、window backend、threaded frontend 之類的 compile-time 結構都留著，只是讓 guest 看不到那兩個裝置。
- 結果系統可以越過原本的 panic 點，表示 trigger 確實和那兩個 `virtio,mmio` node 的存在有關。

---

### 5. 把 input node 改成 dummy compatible

實驗前的暫時修改:

- 把 `minimal.dts` 兩個 input node 的 `compatible = "virtio,mmio"` 改成 `compatible = "semu,dummy-input"`

執行指令:

```bash
make -j4 semu minimal.dtb
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    3.915110] virtio_blk virtio1: 1/0/0 default/read/poll queues
[    3.948839] virtio_net virtio0: Assigned random MAC address
[    4.399019] Run /init as init process
[*] Attempting to mount /dev/vda
```

之後沒有出現原本那個 `__kmalloc_cache_noprof -> vm_find_vqs` panic，而是更晚才看到 RCU stall:

```text
[   25.426262] rcu: INFO: rcu_sched detected stalls on CPUs/tasks:
[   35.556454] NMI backtrace for cpu 1 skipped: idling at default_idle_call+0x30/0x48
```

判斷:

- 「只是多兩個 DT node」這個解釋不成立。
- 必須要讓 Linux 真的把這兩個 node 當成 `virtio,mmio` 來處理，才會穩定把早期 panic 觸發出來。

---

### 6. 檢查 SBI IPI / RFENCE handler

我檢查了 [`main.c`](../main.c) 的 SBI handler，特別是:

- `handle_sbi_ecall_IPI()`
- `handle_sbi_ecall_RFENCE()`

然後掛了暫時性 trace，再跑:

```bash
make -j4 semu minimal.dtb
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
sbi-trace: IPI hart=1 mask=0x1 base=0 n_hart=4
sbi-trace: IPI hart=0 mask=0x1 base=0x1 n_hart=4
sbi-trace: IPI hart=2 mask=0x1 base=0 n_hart=4
sbi-trace: IPI hart=0 mask=0x1 base=0x2 n_hart=4
sbi-trace: IPI hart=3 mask=0x1 base=0 n_hart=4
sbi-trace: RFENCE.I hart=0 mask=0xf base=0 n_hart=4
```

判斷:

- 在這輪 trace 裡沒有看到奇怪的 `hart_mask_base`，也沒有 out-of-range。
- 這讓「目前 crash 是因為 IPI / RFENCE mask 算錯，直接 OOB 寫壞記憶體」這條線優先度下降。

---

### 7. 檢查 guest 早期 boot 是否真的有在打 `aq/rl` / `FENCE`

我暫時在 [`riscv.c`](../riscv.c) 的:

- `op_amo()`
- `RV32_MISC_MEM` (`FENCE` / `FENCE.I`)

加 trace，然後跑:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
amo-trace: pc=0xc000ffec insn=0x46e7a62f funct5=8 aqrl=3 hart=0
fence-trace: pc=0xc047d66c insn=0x220000f funct3=0 hart=0
amo-trace: pc=0xc005fa58 insn=0x1b36a72f funct5=3 aqrl=1 hart=0
fence-trace: pc=0xc005fa60 insn=0x330000f funct3=0 hart=0
amo-trace: pc=0xc005fa9c insn=0x1b37262f funct5=3 aqrl=1 hart=0
fence-trace: pc=0xc005faa4 insn=0x330000f funct3=0 hart=0
amo-trace: pc=0xc005ed78 insn=0x1b36a72f funct5=3 aqrl=1 hart=0
fence-trace: pc=0xc005ed80 insn=0x330000f funct3=0 hart=0
```

判斷:

- 這證明 guest 在 very early boot 就真的有在執行帶 `aq/rl` 的 AMO/LR/SC 和 `FENCE`。
- semu 現在在 [`riscv.c`](../riscv.c) 第 980-1020 行沒有處理 `aq/rl` 位元，而 [`riscv.c`](../riscv.c) 第 1112-1117 行直接把 `FENCE/FENCE.I` 當 no-op。
- 這代表「ordering 指令在 guest 裡是活的」，不是死碼。

---

### 8. 用 `gdb-multiarch` 看 guest kernel 的實際 fast path

先查 symbol 是否存在:

```bash
gdb-multiarch -q -batch \
  -ex 'set architecture riscv:rv32' \
  -ex 'file linux/vmlinux' \
  -ex 'info address __update_cpu_freelist_fast' \
  -ex 'info address try_cmpxchg_freelist' \
  -ex 'info address system_has_freelist_aba'
```

輸出:

```text
The target architecture is set to "riscv:rv32".
No symbol "try_cmpxchg_freelist" in current context.
No symbol "system_has_freelist_aba" in current context.
Symbol "__update_cpu_freelist_fast" is a function at address 0xc00f3d3c.
```

判斷:

- `__update_cpu_freelist_fast` 真的存在，表示這顆 guest kernel 的 SLUB percpu fast path 有編進來。

接著反組譯:

```bash
gdb-multiarch -q -batch \
  -ex 'set architecture riscv:rv32' \
  -ex 'file linux/vmlinux' \
  -ex 'disassemble /r __update_cpu_freelist_fast'
```

關鍵輸出:

```text
Dump of assembler code for function __update_cpu_freelist_fast:
   0xc00f3d4c <+16>: 10017773         csrrci a4,sstatus,2
   0xc00f3d50 <+20>: 01022803         lw a6,16(tp)
   0xc00f3d58 <+32>: 1b850513         addi a0,a0,440 # ... <__per_cpu_offset>
   0xc00f3d74 <+56>: 02b51a63         bne a0,a1,...
   0xc00f3d80 <+68>: 00168893         addi a7,a3,1
   0xc00f3d84 <+72>: 00c7a023         sw a2,0(a5)
   0xc00f3d88 <+76>: 0117a223         sw a7,4(a5)
   0xc00f3d94 <+88>: 1007a073         csrs sstatus,a5
```

判斷:

- 這條 fast path 在 RV32 上不是靠 LR/SC 做 freelist cmpxchg。
- 它更像是:
  - 先關 `sstatus.SIE`
  - 透過 `tp` + `__per_cpu_offset` 找目前 CPU 的 percpu 區
  - 更新 freelist / tid
  - 再恢復中斷
- 所以對這次 crash 來說，比起單純 `aq/rl` 沒實作，現在更可疑的是:
  - local interrupt masking 語意
  - percpu current-cpu / `tp` / hart 語意

---

### 9. 幫 trace 到的 PC 做 symbol lookup

執行指令:

```bash
gdb-multiarch -q -batch \
  -ex 'set architecture riscv:rv32' \
  -ex 'file linux/vmlinux' \
  -ex 'info symbol 0xc000ffec' \
  -ex 'info symbol 0xc047d66c' \
  -ex 'info symbol 0xc005f968' \
  -ex 'info symbol 0xc005fa58' \
  -ex 'info symbol 0xc005fa60' \
  -ex 'info symbol 0xc005fa9c' \
  -ex 'info symbol 0xc005faa4' \
  -ex 'info symbol 0xc005ed78' \
  -ex 'info symbol 0xc005ed80' \
  -ex 'info symbol 0xc005ef10' \
  -ex 'info symbol 0xc005ef18' \
  -ex 'info symbol 0xc005efc0' \
  -ex 'info symbol 0xc005eb0c' \
  -ex 'info symbol 0xc005eb54'
```

輸出:

```text
set_cpu_online.part + 48 in section .text
sched_clock_noinstr + 116 in section .text
prb_reserve + 144 in section .text
prb_reserve + 384 in section .text
prb_reserve + 392 in section .text
prb_reserve + 452 in section .text
prb_reserve + 460 in section .text
data_alloc + 140 in section .text
data_alloc + 148 in section .text
_prb_commit + 76 in section .text
_prb_commit + 84 in section .text
desc_last_finalized_seq + 24 in section .text
desc_read + 132 in section .text
desc_read + 204 in section .text
```

判斷:

- 我這次 trace 到的最前面幾個 `aq/rl` / `FENCE` 位址，大多落在 early boot 的 ringbuffer / printk / CPU online 路徑。
- 這說明 ordering 指令在 early boot 確實很活躍。
- 但它們不是直接把問題縮到 SLUB 本身的唯一鐵證，所以這一點我只把它當成 supporting evidence，不把它講成 final proof。

---

### 10. 嘗試用 `slub_debug` 把 SLUB 拉離 fast path

實驗前的暫時修改:

- 把 `minimal.dts` 的 bootargs 改成:

```text
earlycon console=ttyS0 slub_debug=FZPU
```

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.000000] Kernel command line: earlycon console=ttyS0 slub_debug=FZPU
[    0.000000] Unknown kernel command line parameters "slub_debug=FZPU", will be passed to user space.
```

之後仍然在原本附近 panic:

```text
[    0.947738] Unable to handle kernel paging request at virtual address 72697636
[    0.964548] [<c00f68a0>] __kmalloc_cache_noprof+0x9c/0x128
[    0.965762] [<c026f490>] vm_find_vqs+0x188/0x408
```

判斷:

- 這顆 kernel 沒有接受 `slub_debug=FZPU`，所以這個實驗無法用來驗證「關掉 SLUB fast path 後是否消失」。
- 它唯一提供的有效資訊是: 目前不能靠 bootarg 直接把這顆 kernel 拉進 `CONFIG_SLUB_DEBUG` 路徑。

---

### 11. 檢查 guest `__update_cpu_freelist_fast` 的 percpu CPU identity

實驗前的暫時修改:

- 在 semu 的 `vm_step()` 入口，偵測 guest PC 是否等於 `__update_cpu_freelist_fast`
- 讀取 guest `tp`
- 再用 `mmu_load(tp + 16)` 讀出 guest 認知的 current CPU
- 印出:
  - `hart->mhartid`
  - `tp`
  - `tp_cpu`
  - `sstatus_sie`
  - `sip/sie`

先跑一版「全部都印」，結果前 32 筆都被 hart0 用完，而且 `tp_cpu=0`，沒有不一致。

之後把條件改成只印:

- `hart != 0`
- 或 `tp_cpu != hart`

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.050312] smp: Brought up 1 node, 4 CPUs
slub-trace: hart=1 pc=0xc00f3d3c tp=0xc0848700 tp_cpu=1 s_mode=1 sie=1 sip=0 csr_sie=0x22 ...
slub-trace: hart=2 pc=0xc00f3d3c tp=0xc08e9500 tp_cpu=2 s_mode=1 sie=1 sip=0 csr_sie=0x22 ...
```

而直到 crash 為止，沒有看到:

- `tp_cpu != hart`

判斷:

- 至少在 `__update_cpu_freelist_fast` 這條 guest SLUB percpu fast path 上，hart1 / hart2 的 current CPU identity 是一致的。
- 所以「wrong percpu CPU identity」不是目前最直接的根因。
- 這條線不能完全排除，但優先度明顯下降。

---

### 12. 保留 `virtio,mmio` node，但讓 host 回傳 `DeviceID = 0`

實驗前的暫時修改:

- 在 `virtio-input.c` 的 `DeviceID` register read 暫時回傳 `0`，而不是 `18`

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.933857] virtiofs virtio4: probe with driver virtiofs failed with error -2
[    3.263206] virtio_blk virtio1: 1/0/0 default/read/poll queues
[    3.274102] virtio_blk virtio1: [vda] 2097152 512-byte logical blocks
[    3.297960] virtio_net virtio0: Assigned random MAC address
[    3.390360] NET: Registered PF_PACKET protocol family
```

在這個控制組裡，系統明顯越過了原本 `0.93s` 左右就會發生的 panic 點，沒有再出現:

```text
Unable to handle kernel paging request at virtual address 72697636
```

判斷:

- 這個結果很重要。
- 它表示「只是有 `virtio,mmio` node」還不夠，真正的 trigger 至少要走到 Linux 把它當成有效 `virtio` device 建起來之後。
- 結合前面「拿掉 IRQ 仍會 later crash」的實驗，這比較像:
  - 只要成功走進 `virtio device` 建立與其伴隨的 driver-core/devtmpfs/alloc 工作
  - 就足以把 latent bug 壓出來
- 但它仍然不能單獨證明是 `virtio_input` host MMIO handler 直接寫壞 guest RAM。

---

### 13. 正常 `DeviceID=18`，但用 `quiet loglevel=0` 壓低 printk

實驗前的暫時修改:

- 把 `DeviceID` 還原成 `18`
- 把 `minimal.dts` bootargs 暫時改成:

```text
earlycon console=ttyS0 quiet loglevel=0
```

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.000000] earlycon: ns16550 at MMIO 0xf4000000 (options '')
[    0.000000] printk: legacy bootconsole [ns16550] enabled
```

之後幾乎沒有後續 kernel log，但 process 持續存活，並且至少已經越過原本固定在 `0.93s` 左右發生的 panic window。換句話說:

- 在正常 `DeviceID=18` 下
- 僅靠壓低 printk
- 原本的早期 panic 也被明顯延後或避開了

判斷:

- 這是新的強訊號。
- 它說明:
  - 讓系統少做一點早期 printk / 相關工作
  - 也能壓掉原本的早期 crash
- 這個結果有兩種可能解讀:
  1. 問題真的跟 printk/ringbuffer/其同步路徑有關
  2. 更保守地說，額外的 printk 只是改變了 SMP 下各 hart 的相對進度與 interleaving，從而放大 latent bug
- 因為前面 trace 到的 `aq/rl` / `FENCE` 位址確實大量落在 `prb_reserve` / `_prb_commit` / `desc_read` 這些 ringbuffer 路徑，這條線目前的嫌疑有上升。
- 但目前還不能把它直接定義成 final root cause，只能說它是很強的關聯線索。

---

### 14. 把 SMP coroutine batch 從 `64` 暫時降成 `1`

實驗前的暫時修改:

- 把 `main.c` 裡 `hart_exec_loop()` 的 `for (int i = 0; i < 64; i++)` 暫時改成 `for (int i = 0; i < 1; i++)`

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.883584] NET: Registered PF_INET protocol family
[    1.123231] riscv-plic: interrupt-controller@0: mapped 31 interrupts with 4 handlers for 4 contexts.
[    1.172771] virtiofs virtio4: probe with driver virtiofs failed with error -2
[    1.189346] Unable to handle kernel paging request at virtual address 72697636
[    1.191807] CPU: 3 UID: 0 PID: 1 Comm: swapper/0
```

對照原本正常 batch=`64` 的行為:

- 原本 panic 大多固定在 `~0.93s`
- 改成 batch=`1` 後，panic 明顯延後到 `~1.19s`
- bad pointer 仍然是同一組 `0x7269762e / 0x72697636`
- stack 仍然是同一條 `__kmalloc_cache_noprof -> vm_find_vqs -> virtinput_init_vqs`

判斷:

- 這個結果很有價值，但不是決定性翻案。
- 它說明「hart 交錯粒度」會影響 panic 何時發生，代表 timing / interleaving 確實參與了 trigger。
- 但它也同時說明，根因不只是「batch 太大」。因為即使把 batch 壓到 `1`，panic 仍然存在，只是往後延。
- 所以這條線比較支持:
  - 存在一個對 interleaving 敏感的 latent SMP bug
  - `64` instructions/batch 只是讓它更早、更穩定地被打出來
- 它不支持:
  - 單純把 batch 調小就能解決問題
  - 或是 `virtio-input` MMIO 一碰就 deterministic 直接寫壞 RAM

---

### 15. 只抓「interrupt 即將送進 guest，但 guest PC 還在 critical section」的窄 trace

實驗前的暫時修改:

- 在 `riscv.c` 的 `vm_step()` interrupt 檢查前，加一個極窄 trace
- 只在以下區間、且 `(sip & sie) != 0` 即將進 `hart_trap()` 時才印:
  - `__update_cpu_freelist_fast` critical section: `0xc00f3d50 .. 0xc00f3d94`
  - `prb_reserve` local-interrupt-disabled region: `0xc005f93c .. 0xc005f9ac`

我先用 guest `linux/vmlinux` 符號/反組譯確定了這兩段範圍:

```bash
nm -n linux/vmlinux | rg " (__update_cpu_freelist_fast|prb_reserve|_prb_commit|desc_read|data_alloc|desc_last_finalized_seq|sched_clock_noinstr)$"
gdb-multiarch -q -batch -ex 'set architecture riscv:rv32' -ex 'file linux/vmlinux' -ex 'disassemble /r __update_cpu_freelist_fast'
gdb-multiarch -q -batch -ex 'set architecture riscv:rv32' -ex 'file linux/vmlinux' -ex 'disassemble /r prb_reserve'
```

符號位址:

```text
c005ea88 t desc_read
c005ecec t data_alloc
c005eec4 t _prb_commit
c005efa8 t desc_last_finalized_seq
c005f8d8 T prb_reserve
c00f3d3c t __update_cpu_freelist_fast
c047d5f8 T sched_clock_noinstr
```

關鍵反組譯片段:

```text
0xc00f3d4c <+16>: csrrci a4,sstatus,2
...
0xc00f3d94 <+88>: csrs    sstatus,a5

0xc005f938 <+96>: csrrci a5,sstatus,2
...
0xc005f9ac <+212>: csrs   sstatus,a5
```

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.933857] virtiofs virtio4: probe with driver virtiofs failed with error -2
[    0.949503] Unable to handle kernel paging request at virtual address 72697636
```

而在整次 run 裡，完全沒有出現任何:

```text
critical-irq-trace: ...
```

判斷:

- 這表示在我監看的兩段 guest critical section 內，至少沒有觀察到「semu 正要送 interrupt trap 進去」的情況。
- 所以目前沒有證據支持:
  - `__update_cpu_freelist_fast` 被 local interrupt 錯誤打斷
  - 或 `prb_reserve` 在關本地中斷期間仍被 semu 插入 interrupt trap
- 這讓「local interrupt delivery 明顯壞掉」的假說再往下降一點。
- 但它仍不能完全排除更廣義的 SMP ordering / progress 問題，因為:
  - 這個 trace 只看得到 interrupt trap
  - 看不到例如 lock-free path 本身的 memory-order / progress anomaly
  - 也看不到 non-interrupt 類型的 interleaving 問題

## 綜合判斷依據

### 可以直接觀察到的事實

- panic 固定落在 guest allocator / virtio probe 期間
- bad pointer 長得像 stale data，不像合法 pointer
- 拿掉 input node 後不會在原位置炸
- 改成 dummy compatible 後也不會在原位置炸
- 拿掉 IRQ property 後仍然會 later 用同樣壞 pointer 爆掉
- SBI IPI/RFENCE trace 沒看到異常 mask/base
- guest early boot 的確有大量 ordering 指令
- guest `__update_cpu_freelist_fast` 在 RV32 上是 local-interrupt + percpu path
- `__update_cpu_freelist_fast` 的 `tp_cpu` 與 `hart` 在 hart1/hart2 上可對得起來
- 讓 `DeviceID=0` 後，早期 panic 消失
- 在正常 `DeviceID=18` 下，只靠 `quiet loglevel=0` 也能把早期 panic 壓掉
- 把 SMP batch 從 `64` 降到 `1` 後，panic 只延後、不會消失
- 在 `__update_cpu_freelist_fast` 和 `prb_reserve` 的監看 critical section 內，沒有觀察到 interrupt trap 被送進去

### 我據此排掉的方向

- `window-events` / event delivery race
- `virtio-input` 真正送 key/mouse event 到 guest buffer 的邏輯
- 單純的 PLIC IRQ 7/8 編號問題
- 單純的 `request_irq()` / IRQ registration 問題
- 單純的「多兩個 DT node 就炸」
- 明顯的「wrong percpu CPU identity」

### 目前最像的推測

最像的模型是:

1. `vinput-pr` 新增的兩個 `virtio,mmio` input 裝置，讓 Linux 多走了一段早期 driver-core / devtmpfs / slab / percpu 初始化與 probe 流程。
2. 這段額外工作把 semu 原本潛藏的 `SMP=4` 問題穩定引爆。
3. 這個底層問題目前最像和:
   - 早期 printk/ringbuffer 同步路徑
   - 或更廣義的 SMP ordering / interrupt / hart interleaving 語意不完整
   有關。
4. `aq/rl` / `FENCE` 沒完整實作依然是明確存在的 correctness gap，而且它們 trace 到的位址剛好又大量落在 printk/ringbuffer 路徑，這使它們的嫌疑重新上升。
5. 但即使如此，目前我還是把「這是 timing/interleaving 放大的 latent SMP bug」放在比「virtio-input 直接寫壞 RAM」更高的位置。

## 目前的假說排序

### 假說 A: early printk / ringbuffer 相關同步路徑被 semu 的 SMP 語意缺口放大

目前優先度最高。

理由:

- `quiet loglevel=0` 之後，原本固定在 `0.93s` 左右的 panic 被明顯壓掉
- 先前 trace 到的 `aq/rl` / `FENCE` 位址大量落在 `prb_reserve` / `_prb_commit` / `desc_read`
- 額外 `virtio-input` device 帶來的 printk / probe / driver-core 工作量，和 crash 觸發高度相關

### 假說 B: 一般性的 SMP interleaving / timing 問題，printk 只是最容易把它放大的 trigger

目前優先度也很高，和假說 A 很接近。

理由:

- `DeviceID=0` 會讓早期 panic 消失
- `quiet loglevel=0` 也會讓早期 panic 消失
- `batch=1` 只會讓 panic 延後，不會消失
- 兩者共同點是都降低了 early boot 的工作量與 hart 間 interleaving 壓力
- 這也可以解釋為什麼 `master` 還撐得住，但 `vinput-pr` 會比較容易炸

### 假說 C: local interrupt / percpu 語意有洞

目前優先度中等，但比先前下降。

理由:

- `__update_cpu_freelist_fast` 的確依賴 `sstatus.SIE`
- 但 `tp_cpu` 與 `hart` 目前看起來是一致的
- 也還沒有直接看到「critical section 被錯誤中斷打斷」的證據

### 假說 D: `virtio-input` 裝置實作本身直接寫壞 guest 記憶體

目前優先度低。

理由:

- 拿掉 IRQ property 後，即使 probe 失敗，後面仍然會 later 用同一組壞 pointer 爆掉
- `DeviceID=0` 主要是讓 Linux 不再建立有效 virtio device，而不是修掉 host 端某個 ring write
- 問題更像是 probe 帶來的系統性壓力，而不是特定一筆 host MMIO 寫壞 RAM

## 下一步如果要繼續追

我會優先做這兩件事:

1. 在 semu 端 trace:
   - `hart_trap()` 是否會在 guest critical section 內發生不該有的 interrupt trap
   - pending interrupt 何時被注入
2. 再做一組更直接的 printk/ringbuffer 對照:
   - 看 crash 是否真和 `prb_reserve` / `_prb_commit` 附近的同步路徑有關
3. 若還是需要更強證據，再回頭追:
   - `aq/rl`
   - `FENCE`
   - 以及它們對 early boot ringbuffer/CPU-online path 的影響

## 清理狀態

這份紀錄完成時:

- 暫時加上的 trace 已移除
- `minimal.dts` 已還原
- `make check` / `semu` 殘留 process 已清掉

如果之後要繼續追，建議先從「`__update_cpu_freelist_fast` 所依賴的 local interrupt / percpu 語意」下手，不要再先花時間在 PLIC IRQ 編號或 `virtio-input` event path 上。

---

## Session 2 調查 (2026-03-22, Claude Opus 4.6)

### 已排除的假說

#### 1. SBI IPI ssip clearing bug (已修正但 crash 仍在)
- **Bug**: `main.c:506` 的 `data->sswi.ssip[i] = hart_mask & 1` 會把 hart_mask 裡為 0 的 hart 的 pending IPI 清掉。
- **修正**: 改為 `if (hart_mask & 1) data->sswi.ssip[i] = 1;` (只 SET，不 CLEAR)。
- **結果**: 修正後 crash 仍然出現。這個 bug 是真的 correctness bug（應該要 merge），但不是這次 crash 的 root cause。

#### 2. TLB stale entry (已用 DISABLE_TLB 排除)
- **假說**: 某個 hart 有 stale TLB entry，load/store 命中錯誤的 physical page。
- **測試**: 在 `riscv.c` 加入 `#define DISABLE_TLB 1`，完全跳過 load/store TLB cache lookup 和 fill，每次 access 都做 full page table walk。
- **結果**: crash 仍然出現，完全相同的 pattern。
- **結論**: data TLB 不是 root cause。corruption 來自 physical RAM 本身或 page table walk 本身。

### 關鍵發現

#### 3. 0x7269762e 的來源
- 在 kernel Image 裡搜尋 byte pattern `2e 76 69 72` (.vir LE)：
  - **找到**: Image offset `0x4e768e`，是字串 `"virtio_gpu.virglhack"` 的一部分
  - 但 physical page `0x4e7` (含此 rodata) 和被 corrupt 的 slab page `0x805` 是**不同的 physical page**

#### 4. Watchpoint 結果
- 在 `mmu_load` 加入 watchpoint: 如果 `width == RV_MEM_LW && *value == 0x7269762e` 就 log
- 在 `mmu_store` 加入 watchpoint: 如果 `width == RV_MEM_SW && value == 0x7269762e` 就 log
- **讀結果**:
  ```
  [WATCH] hart0 pc=c04765ac LW vaddr=c0805ec8 paddr=00805ec8 val=0x7269762e tlb=hit
  [WATCH] hart0 pc=c04765ac LW vaddr=c0805ef8 paddr=00805ef8 val=0x7269762e tlb=hit
  [WATCH] hart0 pc=c04765ac LW vaddr=c0805f28 paddr=00805f28 val=0x7269762e tlb=hit
  ...（共 29 次，地址間隔 0x30 = slab object size）
  ```
- **寫結果**: 全部是 memcpy 把已經存在的 0x7269762e propagate 到其他 address
  ```
  [WATCH-W] hart0 pc=c04765b4 SW vaddr=c0805ed8 paddr=00805ed8 val=0x7269762e
  ```
- `pc=c04765ac` / `c04765b4` 在 kernel 的 `memcpy` 函數裡（System.map: `c04764b4 W memcpy`）

#### 5. 關鍵觀察
- physical page `0x805` 在 memcpy 讀取前**已經**包含 0x7269762e
- mmu_store 的 SW watchpoint **沒有** catch 到對 0x00805ec8 的初始寫入
- 這表示 0x7269762e 是透過以下方式之一寫入的:
  - (a) byte/halfword 寫入（SB/SH，不是 SW，所以 watchpoint 沒抓到）
  - (b) 不經過 mmu_store 的直接 RAM 寫入（如 VirtIO DMA 或 PTE A/D bit update）
  - (c) 某種我們還沒想到的路徑

### Crash 詳細資訊（可重現）
```
badaddr: 72697636   cause: 0000000d (load page fault)
s1: 7269762e (corrupted freelist pointer)
a5: 72697636 (= s1 + 8 = freelist + kmem_cache->offset)
CPU: 0   PID: 1   Comm: swapper/0

Call stack:
  __kmalloc_cache_noprof+0x9c/0x128
  vm_find_vqs+0x188/0x408
  virtinput_init_vqs+0x70/0x98
  virtinput_probe+0x70/0x528
  virtio_dev_probe+0x220/0x2a0
  ...
  virtio_input_driver_init+0x24/0x34
  do_one_initcall+0x6c/0x25c
  kernel_init_freeable+0x21c/0x220
```

### 目前代碼狀態
- `main.c:504-508`: IPI ssip fix 已套用（`if (hart_mask & 1)` guard）
- `riscv.c`: DISABLE_TLB 已移除，load/store watchpoint 還在
- 其他檔案未修改

### 下一步建議
1. **擴大 write watchpoint**: 在 `ram_write` (ram.c) 加入 watchpoint，抓任何大小的寫入（包含 SB/SH）— 這可以抓到 memcpy 以外的寫入源
2. **追蹤 PTE A/D bit 寫入**: `mmu_translate` 裡的 `*pte_ref = new_pte` 是直接寫 RAM 的，不經過 mmu_store/ram_write。加入 watchpoint 確認這些寫入是否指向錯誤地址
3. **追蹤 VirtIO DMA 寫入**: 檢查所有 VirtIO device 的 descriptor handler 是否有直接寫入 guest RAM 的路徑
4. **考慮 PTE corruption**: 如果 TLB 不是問題，可能是 PTE 本身被 corrupt。加入 page table walk 的 validation: 在 mmu_translate 完成後，驗證 returned physical page 是否合理
5. **字串 "virtio_gpu.virglhack" 的傳播路徑**: 這個 rodata 字串可能透過 kernel 的 `__setup` / module param 機制被 copy 到某個 buffer，而這個 buffer 所在的 page 後來被 free 並 reuse 為 slab page

---

## Session 3 調查 (2026-03-22, Claude Opus 4.6, 續)

### 已排除的假說

#### 16. PTE A/D bit 更新造成 corruption
- **假說**: `mmu_translate` 裡的 `*pte_ref = new_pte` 直接寫 RAM，可能寫出 0x7269762e。
- **分析**: 合法 PTE 的 bit 0 (Valid flag) 必定為 1。但 0x7269762e 的 bit 0 = 0。所以 PTE A/D 更新 **不可能** 產生 0x7269762e。
- **結論**: 已排除。

#### 17. VirtIO DMA 在 probe 階段寫壞 RAM
- **假說**: virtio-input 的 DMA path（直接寫 ram[]）可能在 probe 期間寫壞 guest 記憶體。
- **分析**: DMA 寫入路徑（`virtio_statusq_drain`, `virtio_input_desc_handler`）需要 `Status & DRIVER_OK` 且 `queue->ready`。在 probe 階段，DRIVER_OK 尚未設定。CI mode 下也沒有 window events。
- **結論**: probe 期間不會有 DMA 寫入，已排除。

### 關鍵新發現

#### 18. 初始 corruption 來源確認: `string_nocheck` (vsnprintf)
- **方法**: ram.c 加入 post-write watchpoint，任何大小的寫入（包含 SB/SH）只要導致 cell == 0x7269762e 就 log。
- **結果**:
  - pc=c046ee38 (實際 SB 在 c046ee34)，函數 `string_nocheck + 92`
  - 這是 vsnprintf 字串格式化函數，逐 byte 寫入字串內容
  - 寫入的 byte 是 0x72 ('r')，完成了 `.vir` (LE: 2e 76 69 72 = 0x7269762e)
  - 目標 page 是 0x8e5
- **判斷**: 這是 **合法的** 字串格式化操作。問題不在 string_nocheck 本身，而是它的目標 buffer 和 slab metadata 重疊。

#### 19. 完整 corruption 鏈
```
1. string_nocheck (pc=c046ee34) 逐 byte 寫 ".vir..." 到 page 0x8e5
   → 這是 vsnprintf 格式化含 "virtio" 的字串

2. kstrdup (ra=c00c2290) 呼叫 memcpy (pc=c04765ac)
   → 從 page 0x8e5 複製字串到 slab object (page 0x805)
   → kstrdup 分配 buffer + 複製字串，完全正當的操作

3. __kmalloc_cache_noprof (pc=c00f68a0) 從 page 0x805 讀取 freelist pointer
   → 讀到 0x7269762e，嘗試 dereference → crash
   → badaddr=0x72697636 = 0x7269762e + 8 (kmem_cache->offset)
```

#### 20. Master PLIC context 計算已確認 BUGGY（數學證明）

Master 分支使用的 bit-manipulation formula 結果：

| Hart | Enable addr (word) | 正確 context | Master context |
|------|--------------------|-------------|----------------|
| 0    | 0x800              | 0           | 0 ✓            |
| 1    | 0x820              | 1           | 0 ✗            |
| 2    | 0x840              | 2           | 1 ✗            |
| 3    | 0x860              | 3           | 1 ✗            |

影響：
- `plic_update_interrupts` 使用 `ie[i]` 做中斷 dispatch
- Master 上 ie[2] 和 ie[3] 永遠為 0（沒有 hart 寫過）
- **Hart 2, 3 在 master 上永遠收不到 PLIC 外部中斷**
- vinput-pr 修正後，所有 hart 正確收到中斷

#### 21. Master IPI handler 的額外 bugs

**Bug 1**: `hart_mask_base == 0xFFFFFFFFFFFFFFFF` 永遠不成立
- `(uint64_t) hart->x_regs[RV_R_A1]` 把 32-bit 的 0xFFFFFFFF zero-extend 成 0x00000000FFFFFFFF
- 不等於 0xFFFFFFFFFFFFFFFF
- "all harts" IPI 在 master 上永遠不會觸發

**Bug 2**: IPI handler 會意外清除 pending IPI
- `data->sswi.ssip[i] = hart_mask & 1` 在 mask bit 為 0 時會 **CLEAR** 其他 hart 的 pending IPI
- vinput-pr 已修正為 `if (hart_mask & 1) data->sswi.ssip[i] = 1;`

#### 22. TP (task pointer) 追蹤結果

在 `set_dest` 函數加入追蹤，記錄 secondary hart 每次 tp 寫入：

```
[TP-TRACE] hart1 pc=000000ec tp: 00000000 -> c084f700   # 初始設定（secondary boot code）
[TP-TRACE] hart2 pc=000000ec tp: 00000000 -> c08b0000   # 初始設定
[TP-TRACE] hart3 pc=000000ec tp: 00000000 -> c08b0700   # 初始設定

# context switch 模式 (__switch_to + 128)
[TP-TRACE] hart2 pc=c0484c7c tp: c08b0000 -> c0848000   # 切換到 PID 1 的 task_struct
[TP-TRACE] hart2 pc=c0484c7c tp: c0848000 -> c08b0000   # 切回 idle task

# exception entry/exit (handle_exception)
[TP-TRACE] hart2 pc=c04849c4 tp: c0848000 -> 00000000   # 儲存 tp 到 sscratch
[TP-TRACE] hart2 pc=c04849cc tp: 00000000 -> c0848000   # 從 sscratch 恢復 tp
```

**關鍵發現**:
- 每個 secondary hart 的初始 tp 都是唯一的（c084f700, c08b0000, c08b0700），和 hart0 不同
- Hart2 通過正常的 `__switch_to` context switch 取得 c0848000（= PID 1 task_struct）
- 這看起來是**正常的 task migration**，Linux scheduler 把 PID 1 從 hart0 遷移到 hart2
- 函數確認：
  - `c0484c7c` = `__switch_to + 128`（context switch 載入新 tp）
  - `c047fa5c` = `__schedule + 936`（scheduler 呼叫 __switch_to）
  - `c04849c4` = `handle_exception + 4`（exception entry 儲存 tp）
  - `c04849cc` = `handle_exception + 12`（exception entry 恢復 tp）

#### 23. TP 重疊檢查

追蹤所有 hart 對 c0848000 的 switch-to/switch-from 事件，用 awk 檢查是否有兩個 hart 同時持有 c0848000：

**結果**: **沒有發現重疊**。每次 hart2 取得 c0848000 時，hart0 已經 switch away。

**判斷**:
- "兩個 hart 同時跑同一個 task" 的假說目前**沒有直接證據**支持
- Task migration 本身看起來是正常的
- 但在 migration 過程中，percpu slab metadata 的一致性仍可能有問題

### CSR / 中斷 / AMO 分析

#### 24. CSR sstatus.SIE 處理正確
- `csrrci a4, sstatus, 2` 正確 disable SIE
- `csrs sstatus, a5` 正確 restore SIE (使用 `andi a5, a4, 2` 提取原始 SIE bit)
- `vm_step` 的中斷檢查 `if ((vm->sstatus_sie || !vm->s_mode) && (vm->sip & vm->sie))` 正確尊重 SIE=0

#### 25. LR/SC 實作正確
- LR 設定 `lr_reservation = phys_addr | 1`
- SC 檢查 reservation match，且成功時清除所有 hart 的 reservation
- `mmu_store` 的 reservation clearing 在所有 store（不只 SC）上執行
- cooperative scheduling 下 LR/SC 語意正確（同時間只有一個 hart 執行）

#### 26. AMO aq/rl 忽略但不影響正確性
- `aq`/`rl` bits 在 `op_amo` 中被忽略
- 但在 cooperative scheduling + shared RAM 下，所有 store 立即可見，無 reordering
- FENCE 也是 no-op，同理正確

### 目前代碼狀態
- `main.c:504-508`: IPI ssip fix 已套用
- `riscv.c`: 所有 watchpoint 已移除（WATCH, WATCH-W, TP-TRACE, TP-DUP），回到乾淨狀態
- `ram.c`: WATCH-RAM watchpoint 已移除，`#include <stdio.h>` 保留
- `plic.c`: 正確的 context 計算

### 目前最可能的根因方向

經過 Session 1-3 的深入調查，可以確認的事實：

1. **emulator 的核心語意（CSR, LR/SC, AMO, TLB, interrupt delivery）看起來正確**
2. **PLIC fix 是正確的**，master 是 buggy 的
3. **IPI fix 是正確的**
4. **不存在 "兩個 hart 同時有相同 tp" 的情況**（重疊檢查未發現）
5. **task migration 是正常的 Linux scheduler 行為**
6. **corruption 鏈明確**：string_nocheck → kstrdup/memcpy → slab freelist 被覆蓋

**最可能的根因仍然是 timing/interleaving 相關**：
- 正確的 PLIC 讓所有 hart 收到外部中斷 → 更多 context switch
- 更多 context switch → task migration 更頻繁
- Task migration 期間的 percpu slab 切換可能有 race window

**但在 cooperative scheduling（一次只執行一個 hart）下，理論上不應有 race**。
除非存在某種更微妙的語意缺口，例如：
- 在 hart A 的 batch 中間修改了 hart B 的 sip（plic_update_interrupts 從所有 hart 的 emu_tick_peripherals 調用，修改所有 hart 的 sip）
- 這導致 hart B 在下一個 batch 開始時看到「不應該看到的」中斷狀態

### 下一步建議

1. **重新檢查 `plic_update_interrupts` 的 cross-hart sip 修改**：
   - 在 hart A 執行期間，emu_tick_peripherals 呼叫 plic_update_interrupts
   - 這會修改 **所有 hart** 的 sip
   - 如果 hart B 的 ie[B] 在 hart A 的 batch 中被修改（透過 MMIO 寫入），sip 可能在 batch 中間改變
   - 需要確認這是否會導致 hart B 看到 stale 的中斷狀態

2. **追蹤 slab object 生命週期**：
   - 在 guest `__kmalloc_cache_noprof` 和 `kfree` 入口加 trace
   - 確認被 corrupt 的 slab object 是否被兩個不同的 hart 同時「持有」

3. **嘗試 per-hart peripheral tick**：
   - 修改 emu_tick_peripherals 使其只更新當前 hart 的 sip
   - 這可能消除 cross-hart sip race window
   - 但需要另外確保 interrupt delivery 仍然正確

---

## Session 4 調查 (2026-03-22, Codex)

### 27. PLIC 外部中斷在 early panic 前沒有直接參與

我先在 `plic_update_interrupts()` 加了兩種極窄 trace:

- `plic-multi-target`: 只有當同一輪 `ip` 同時對多個 hart 有效時才印
- `plic-delivery`: 只有當真的有任何 hart 收到 SEI 時才印

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

結果:

- panic 前完全沒有任何 `plic-multi-target`
- panic 前也完全沒有任何 `plic-delivery`

關鍵輸出仍然是原本那條:

```text
[    0.925676] virtiofs virtio4: probe with driver virtiofs failed with error -2
[    0.940943] Unable to handle kernel paging request at virtual address 72697636
```

判斷:

- 至少這個 early panic window 內，`PLIC` 外部中斷根本還沒參與。
- 所以 Session 3 最後那條「PLIC cross-hart sip 修改可能造成早期 crash」的假說，不符合目前觀察。
- 這不代表 PLIC 完全沒問題，只代表它不是這次 `0.94s` 左右 early panic 的直接 trigger。

---

### 28. 初始 `mtimecmp = 0` 會造成 early timer interrupt，但不是主根因

我在 `aclint_mtimer_update_interrupts()` / `aclint_sswi_update_interrupts()` 暫時加了 trace，觀察 early boot 前幾次 interrupt injection。

關鍵輸出:

```text
mtimer-delivery: hart=0 mtime=1 mtimecmp=0 sip_before=0x0
mtimer-delivery: hart=1 mtime=62 mtimecmp=0 sip_before=0x0
mtimer-delivery: hart=2 mtime=64 mtimecmp=0 sip_before=0x0
mtimer-delivery: hart=3 mtime=66 mtimecmp=0 sip_before=0x0
...
sswi-delivery: hart=0 ssip=1 sip_before=0x0
sswi-delivery: hart=0 ssip=1 sip_before=0x0
...
```

這證明:

- `mtimecmp[]` 目前是 `calloc()` 出來的，所以初值為 `0`
- 每個 hart 在 Linux 自己設 timer 之前，就先收到了 pending timer interrupt
- early boot 期間也確實有大量 `SSWI` 打到 hart0

接著我做了更乾淨的對照:

- 在 `main.c` 初始化 `mtimer.mtimecmp` 後，暫時把每個 hart 的初值改成 `UINT64_MAX`

執行同樣的 reproducer 後，結果幾乎沒有變:

```text
[    0.925676] virtiofs virtio4: probe with driver virtiofs failed with error -2
[    0.940943] Unable to handle kernel paging request at virtual address 72697636
```

判斷:

- `mtimecmp` 初值為 0 是一個合理懷疑點，也很可能是 correctness issue
- 但它不是這次 early panic 的主根因，因為把它改成更合理的 `UINT64_MAX` 後，crash timing 和 pattern 幾乎不變

---

### 29. page `0x805` 的那串 object start 在這次 run 只看到 `new_slab` 初始化

根據前一輪紀錄，我一度直接盯住 page `0x805` 上那串 `0x30` 間距的 object start words:

- `0x00805ec8`
- `0x00805ef8`
- `0x00805f28`
- `0x00805f58`
- `0x00805f88`
- `0x00805fb8`
- `0x00805fe8`

關鍵輸出:

```text
ram-watch-write: hart=0 pc=0xc00f54a8 addr=0x00805ec8 width=2 old=0x00000000 value=0xc0805ed0 new=0xc0805ed0
ram-watch-write: hart=0 pc=0xc00f54a8 addr=0x00805ef8 width=2 old=0x00000000 value=0xc0805f00 new=0xc0805f00
...
ram-watch-write: hart=0 pc=0xc00f54a8 addr=0x00805fe8 width=2 old=0x00000000 value=0xc0805ff0 new=0xc0805ff0
```

我把 guest PC `0xc00f54a8` 對到 `linux/vmlinux`:

```text
0xc00f54a8 <new_slab+448>: sw s3,0(s1)
```

判斷:

- 這批寫入是 `SLUB new_slab` 正常初始化 freelist 的行為
- 在這次 run 裡，我**沒有**看到這幾個 page `0x805` slots 之後再被改成 `.vir`
- 也沒有看到 crash 前 allocator 從這批 slots 直接讀出 `0x7269762e`
- 這表示 Claude 先前盯到的 page `0x805` 雖然是真實案例，但**不是每一輪固定都撞同一頁**

這是一個重要的收斂:

- 問題不像「永遠是 page `0x805` 被打壞」
- 更像「allocator 被系統性搞亂後，哪一頁中標會隨 run 漂移」

---

### 30. 這一輪重新確認：`.vir` 來源 page 仍然是 `0x8e5...`，而且這次由 hart2 在 `memcpy` 讀它

我把 watchpoint 改成更一般化:

- 只要 `ram_read()` 的 `LW` 結果等於 `0x7269762e` 或 `0x72697636` 就印

關鍵輸出:

```text
ram-watch-read: hart=2 pc=0xc04765a8 addr=0x008e5ae8 width=2 cell=0x7269762e value=0x7269762e
ram-watch-read: hart=2 pc=0xc04765a8 addr=0x008e5b18 width=2 cell=0x7269762e value=0x7269762e
ram-watch-read: hart=2 pc=0xc04765a8 addr=0x008e5b48 width=2 cell=0x7269762e value=0x7269762e
...
ram-watch-read: hart=0 pc=0xc04765a8 addr=0x008e5ae8 width=2 cell=0x7269762e value=0x7269762e
```

這些位址都落在 page `0x8e5`，和前面 Session 2/3 的觀察一致。

`pc=0xc04765a8` 仍然是在 guest `memcpy` 裡。

而同一次 run 的 crash 是:

```text
[    0.943525] CPU: 0 UID: 0 PID: 1 Comm: swapper/0
[    0.945275] epc : __kmalloc_cache_noprof+0x9c/0x128
```

判斷:

- `.vir` 的來源 page 目前看起來很穩定，仍然是 `0x8e5...`
- 但這次最早把它當成 source 讀出來做 `memcpy` 的是 **hart2**
- 最後真正 crash 的卻是 **hart0**

這點很重要，因為它比前面更具體地支持:

- stale content 的流動和 allocator 最後爆掉，確實可能跨 hart
- 問題不是單一 hart 自己在自己的 freelist 上做錯一件事那麼簡單
- 至少這次 run 裡，source-side string handling 和最後 allocator crash 已經分散在不同 hart

### 目前代碼狀態

- `plic.c` 的暫時 trace 已移除
- `aclint.c` 的暫時 trace 已移除
- `main.c` 的 `mtimecmp = UINT64_MAX` 對照已還原
- `ram.c` 目前保留的是 generic stale-pointer read/write watch，因為下一步要把 destination page 補抓出來

### 更新後的判斷

Session 4 之後，可以更有把握地說:

1. **PLIC 不是這次 early panic 的 trigger**
2. **初始 timer interrupt storm 不是主根因**
3. **corruption target page 不是固定的，至少會隨 run 漂移**
4. **`.vir` source page (`0x8e5...`) 很穩定，但 source-side memcpy 和最後 crash 已經明確跨 hart**

這讓「systemic allocator corruption + cross-hart interleaving/task migration 參與」這條線，優先度又比先前更高了一些。

---

### 31. 這一輪把 stale pointer 寫進 per-CPU freelist 的 commit point 抓出來了

在 Session 4 的 generic stale read/write watch 基礎上，我接著讓 `ram_write()` 也在 cell 寫成 `0x7269762e / 0x72697636` 時印 log。

執行指令:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出可以整理成這條鏈:

```text
# 1. hart2 用 string_nocheck 逐 byte 生成 ".vir"
ram-watch-write: hart=2 pc=0xc046ee34 addr=0x008e5aeb width=0 value=0x00000072 new=0x7269762e

# 2. hart2 在 __pi___memcpy 讀 source page 0x8e5...
ram-watch-read:  hart=2 pc=0xc04765a8 addr=0x008e5ae8 width=2 cell=0x7269762e value=0x7269762e

# 3. hart0 後來也從同一個 source page 讀，並在 __pi___memcpy 寫到 page 0x805...
ram-watch-read:  hart=0 pc=0xc04765a8 addr=0x008e5ae8 width=2 cell=0x7269762e value=0x7269762e
ram-watch-write: hart=0 pc=0xc04765b0 addr=0x00805468 width=2 value=0x7269762e new=0x7269762e

# 4. allocator 隨後在 hart0 讀到了這個 object 裡的 stale value
ram-watch-read:  hart=0 pc=0xc00f6c9c addr=0x00805468 width=2 cell=0x7269762e value=0x7269762e

# 5. 然後 __update_cpu_freelist_fast 直接把它寫進 per-CPU freelist state
ram-watch-write: hart=0 pc=0xc00f3d84 addr=0x1ff4eae0 width=2 value=0x7269762e new=0x7269762e
```

我再把關鍵 PC 對回 guest `linux/vmlinux`:

```text
0xc04765a8 <__pi___memcpy+244>: lw a4,0(a1)
0xc04765b0 <__pi___memcpy+252>: sw a4,0(t6)

0xc00f6c9c <__kmalloc_node_track_caller_noprof+208>: lw a2,0(a5)
0xc00f6ca4 <__kmalloc_node_track_caller_noprof+216>: jal __update_cpu_freelist_fast

0xc00f3d84 <__update_cpu_freelist_fast+72>: sw a2,0(a5)
```

另外，這輪真的被拿來當 stale object 的 destination word 是:

```text
0x00805468
```

它在 physical page `0x805` 上，但不是先前盯的 `0x00805ec8` 那一串 slot，而是同頁另一個 offset (`0x468`)。

判斷:

- 這次已經不只是「某處有 `.vir`，最後 crash」的關聯而已。
- 我現在直接看到:
  1. `__pi___memcpy` 把 `.vir` 寫進 page `0x805` 的 object word (`0x00805468`)
  2. `__kmalloc_node_track_caller_noprof` 之後把這個 word 當成 freelist pointer 讀出來
  3. `__update_cpu_freelist_fast` 再把它 commit 回 per-CPU freelist state

- 這表示「allocator 真正接受 stale string value 當 freelist pointer」的 commit point 已經被抓到了。
- 換句話說，現在最值得追的不是 `virtio-input` 本身，而是:
  - 為什麼 `0x00805468` 這個帶字串內容的 object，會在這個時刻被 allocator 視為可拿來更新 freelist 的 object

這比先前的結論更精確，因為它已經把根因候選收斂到:

1. **object lifecycle / free-list membership 出錯**
2. **而且錯誤發生時，真正 commit stale pointer 的地方就在 guest SLUB fast path**

### 32. `0x00805468` 的 object lifecycle 現在也對上了

我把上面那個真正中標的 destination word `0x00805468` 再做一次更窄的生命周期追蹤，得到這條完整序列:

```text
# A. new_slab 初始建 freelist
ram-watch-write: hart=0 pc=0xc00f54a8 addr=0x00805468 width=2 value=0xc0805470 new=0xc0805470

# B. allocator 一開始確實把它當合法 freelist member 讀過
ram-watch-read:  hart=0 pc=0xc00f71a8 addr=0x00805468 width=2 cell=0xc0805470 value=0xc0805470

# C. 之後 object 被 memset 清掉
ram-watch-write: hart=0 pc=0xc0476878 addr=0x00805468 width=2 value=0x00000000 new=0x00000000

# D. free path 再把它接回 freelist，先後寫入兩個合法 next pointer
ram-watch-write: hart=0 pc=0xc00f5a2c addr=0x00805468 width=2 value=0xc0805450 new=0xc0805450
ram-watch-write: hart=0 pc=0xc00f5a2c addr=0x00805468 width=2 value=0xc0805460 new=0xc0805460

# E. allocator 之後再次把它當合法 freelist member 讀出
ram-watch-read:  hart=0 pc=0xc00f6c9c addr=0x00805468 width=2 cell=0xc0805460 value=0xc0805460

# F. 接著 __pi___memcpy 直接把 ".vir" 蓋進同一個 word
ram-watch-write: hart=0 pc=0xc04765b0 addr=0x00805468 width=2 value=0x7269762e new=0x7269762e

# G. allocator 緊接著又把它當 freelist pointer 讀出
ram-watch-read:  hart=0 pc=0xc00f6c9c addr=0x00805468 width=2 cell=0x7269762e value=0x7269762e
```

我再把中間幾個關鍵 PC 對回 guest symbol:

```text
0xc00f71a8 <__kmalloc_node_noprof+208>: lw a2,0(a5)
0xc0476878 <__pi___memset+228>:        sw a1,120(t0)
0xc00f5a2c <kfree+292>:                sw a1,0(a5)
0xc00f6c9c <__kmalloc_node_track_caller_noprof+208>: lw a2,0(a5)
0xc04765b0 <__pi___memcpy+252>:        sw a4,0(t6)
0xc00f3d84 <__update_cpu_freelist_fast+72>: sw a2,0(a5)
```

這條鏈的意義非常直接:

- `0x00805468` 這個 object **確實曾經是合法 freelist member**
- 它也 **確實被 kfree 接回過 freelist**
- 但在下一次 allocator 消費它之前，`memcpy` 又把 `.vir` 直接寫進同一個 word
- allocator 沒有任何額外檢查，就把這個新內容當成 next pointer 拿去更新 per-CPU freelist

現在最重要的問題變成:

- **為什麼這個 object 在 `memcpy` 當下仍然還是 freelist member**

也就是說，根因已經進一步收斂成兩類:

1. 這個 object 的 **free-list membership 被提早暴露**，導致尚未真正安全重用就被當作 data buffer 寫入
2. 或者這個 object 的 **allocation/free 所屬關係失真**，讓 allocator 和一般 data path 同時相信自己擁有它

不管是哪一種，都比「某個 device 隨機打壞 RAM」更接近目前實際觀察。

### 更新後的判斷

Session 4 到目前為止，最像的模型變成:

- source side 的 `.vir` 字串內容本身是合法寫入
- `memcpy` 也是合法操作
- 真正出問題的是 **某個 object 本來不該在這個時刻被當成 freelist 成員，但 allocator 卻把它當成 freelist object 消費了**

也就是說，問題的核心更像:

- **free-list membership / object ownership 失真**
- 而不是單純「某個 device 直接把 guest RAM 打壞」

### 33. guest `virtio_fs` source 現在已經對出一條具體的 double-free 路徑

這一輪我先不再加新 trace，而是把前面 watchpoint 抓到的 caller，直接對回 guest kernel source。

執行指令:

```bash
nl -ba linux/fs/fuse/virtio_fs.c | sed -n '160,220p'
nl -ba linux/fs/fuse/virtio_fs.c | sed -n '900,1188p'
nl -ba linux/mm/util.c | sed -n '30,90p'
nl -ba linux/fs/kernfs/dir.c | sed -n '500,700p'
```

關鍵 source 片段:

```text
# virtio_fs_ktype_release()
190 static void virtio_fs_ktype_release(struct kobject *kobj)
194     kfree(vfs->mq_map);
195     kfree(vfs->vqs);
196     kfree(vfs);

# virtio_fs_setup_vqs()
947 fs->vqs = kcalloc(...)
952 fs->mq_map = kcalloc_node(nr_cpu_ids, sizeof(*fs->mq_map), ...)
975 ret = virtio_find_vqs(...)
986 if (ret) {
987     kfree(fs->vqs);
988     kfree(fs->mq_map);
989 }

# virtio_fs_probe()
1131 ret = virtio_fs_setup_vqs(vdev, fs);
1132 if (ret < 0)
1133     goto out;
1156 out:
1157     vdev->priv = NULL;
1158     kobject_put(&fs->kobj);

# kstrdup_const / kstrdup
82 const char *kstrdup_const(const char *s, gfp_t gfp)
87     return kstrdup(s, gfp);
64 buf = kmalloc_track_caller(len, gfp);
66 memcpy(buf, s, len);

# kernfs free path
536 kfree_const(kn->name);
620 name = kstrdup_const(name, GFP_KERNEL);
```

把這些 source 跟前面 Session 4 抓到的 object lifecycle 疊起來，現在可以得到一個很具體的解釋:

1. `virtio_fs_setup_vqs()` 在失敗時，已經先 `kfree(fs->vqs)` 和 `kfree(fs->mq_map)`。
2. `virtio_fs_probe()` 隨後仍然會走到 `kobject_put(&fs->kobj)`。
3. `kobject_put()` 最後又會進 `virtio_fs_ktype_release()`，再 `kfree(vfs->mq_map)` / `kfree(vfs->vqs)` 一次。

也就是說，單看 source，這裡就已經存在非常直接的 double-free。

這和前面 watchpoint 抓到的現象非常吻合:

```text
# 第一次 free 後 old_head = 0xc0805450
ram-watch-kfree-caller: ... caller_ra=0xc01f2414 ... old_head=0xc0805450 obj=0xc0805460

# 第二次 free 時 old_head 已經等於 obj 本身
ram-watch-kfree-caller: ... caller_ra=0xc01f0b44 ... old_head=0xc0805460 obj=0xc0805460
```

第二次 free 時出現 `old_head == obj`，正是 self-linked freelist / double-free 的典型形狀。

另外，這個被重複 free 的 object 很像就是 `mq_map`:

- `mq_map` 的大小是 `nr_cpu_ids * sizeof(unsigned int)`
- 這次 boot log 裡 `nr_cpu_ids = 4`
- 所以 `mq_map` 大小正好是 `16 bytes`
- 而前面 `kstrdup_const -> __kernfs_new_node` 那次重用，watchpoint 抓到的 `len=0x10`

這不是百分之百鐵證，但對應度非常高:

- `mq_map` 是小型 kmalloc object
- `vqs` 則是較大的陣列，比較不像會落在這次盯到的 `0x10` 物件上

接著我再把前面 watchpoint 裡那兩個 `caller_ra` 直接反組譯:

執行指令:

```bash
gdb-multiarch -q -batch \
  -ex 'set architecture riscv:rv32' \
  -ex 'file linux/vmlinux' \
  -ex 'x/32i 0xc01f23e0' \
  -ex 'x/16i 0xc01f0b30'
```

關鍵輸出:

```text
0xc01f240c <virtio_fs_probe+876>: lw  a0,68(s1)
0xc01f2410 <virtio_fs_probe+880>: jal 0xc00f5908 <kfree>
0xc01f2414 <virtio_fs_probe+884>: bltz s2,...

0xc01f0b3c <virtio_fs_ktype_release+24>: lw  a0,68(a0)
0xc01f0b40 <virtio_fs_ktype_release+28>: jal 0xc00f5908 <kfree>
0xc01f0b44 <virtio_fs_ktype_release+32>: lw  a0,52(s1)
```

這個反組譯把最後一塊拼圖補上了:

- `caller_ra = 0xc01f2414` 對應的是 **`virtio_fs_probe` 中 `kfree(fs->mq_map)` 返回後**
- `caller_ra = 0xc01f0b44` 對應的是 **`virtio_fs_ktype_release` 中 `kfree(vfs->mq_map)` 返回後**

所以現在已經不是「`mq_map` 很像」而已，而是：

- 前面 watchpoint 盯到的那個 self-linked 小物件，實際上就是 `mq_map`
- 它先在 probe 失敗清理被 free 一次
- 又在 ktype release 再被 free 一次
- 之後才被 `kstrdup_const(__kernfs_new_node)` 重用成 name buffer

所以目前最合理的物件層級推論是:

- `virtio_fs` probe fail path 先 double-free 了一個小型物件，極可能是 `mq_map`
- 這個 object 隨後被 SLUB 重新發給 `kstrdup_const(__kernfs_new_node)` 當 name buffer
- `memcpy` 把 `.vir...` 寫進去後，因為該 object 其實仍然留在 freelist 裡，allocator 之後又把字串當 next pointer 讀出，最後在 `__update_cpu_freelist_fast` commit 掉

這個結論比前面「抽象的 SMP ordering 問題」更直接，也更能解釋為什麼 stale value 會是合法字串內容，而不是隨機 bit flip。

### 34. 暫時拿掉 `virtiofs` node 後，`vinput + SMP=4` 可以越過原本 early panic window

為了確認 `virtiofs probe fail` 這條線是不是必要條件，我做了最小對照:

- 只暫時把 `minimal.dts` 裡的 `fs0: virtio@4800000` node 藏掉
- 保留 `vinput` 兩個 node
- 其他設定不動

臨時修改後執行:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
[    0.937658] input: VirtIO Keyboard as /devices/platform/soc@F0000000/f4900000.virtio/virtio4/input/input0
[    0.953264] input: VirtIO Mouse as /devices/platform/soc@F0000000/f5000000.virtio/virtio5/input/input1
[    3.277049] virtio_blk virtio1: 1/0/0 default/read/poll queues
[    3.311635] virtio_net virtio0: Assigned random MAC address ...
```

而這次**沒有再出現**:

```text
virtiofs virtio4: probe with driver virtiofs failed with error -2
Unable to handle kernel paging request at virtual address 72697636
```

判斷:

- `vinput` 兩個裝置本身可以正常 probe
- 只要把 `virtiofs` 失敗 probe 這條路拿掉，原本固定在 `~0.93s` 左右的 early panic 就跟著消失

這個對照非常關鍵，因為它把目前的主因進一步收斂成:

1. `vinput-pr` 讓系統 early boot 多做了一些 probe / sysfs / allocator 工作，所以更容易踩中
2. 但真正把 freelist 搞壞的最直接來源，現在最像是 guest `virtio_fs` 的 probe failure double-free
3. `vinput` 比較像放大器 / trigger，不像最底層 direct corrupter

### Session 4 之後的更新排序

目前假說排序我會改成:

1. **guest `virtio_fs` probe failure path 的 double-free**
   - 現在優先度最高
   - 已有 source-level 直接證據
   - 也和 watchpoint 的 object lifecycle / self-linked freelist 形狀對得上

2. **`vinput-pr` 改變 early boot 壓力與 interleaving，使這個 latent guest bug 更穩定地炸出來**
   - 這可以解釋為什麼 `master` 在同樣 guest kernel 下不一定馬上炸
   - 也可以解釋為什麼加上兩個 `virtio-input` node 後，問題變得穩定

3. **更廣義的 semu `SMP=4` ordering/timing 問題**
   - 目前仍不能完全排除
   - 但相較於前面這條直接的 guest double-free，優先度已明顯下降

### 35. `virtiofs error -2` 的直接原因也已經抓到，是 semu `virtio-fs` queue 數量和 guest 預期不一致

前面雖然已經知道 `virtiofs` 先 fail probe，才會進 double-free，但還缺最後一塊:

- 為什麼 guest `virtio_fs_probe()` 會先拿到 `-2`？

我先把 guest source 對回去:

```bash
nl -ba linux/fs/fuse/virtio_fs.c | sed -n '924,990p'
nl -ba linux/drivers/virtio/virtio_mmio.c | sed -n '372,520p'
```

關鍵 source:

```text
# guest virtio-fs
938 virtio_cread_le(... num_request_queues, &fs->num_request_queues);
946 fs->nvqs = VQ_REQUEST + fs->num_request_queues;
975 ret = virtio_find_vqs(vdev, fs->nvqs, vqs, vqs_info, &desc);

# guest virtio-mmio
393 writel(index, ... QUEUE_SEL)
396 if (readl(... QUEUE_READY)) {
398     err = -ENOENT;
399     goto error_available;
}
409 num = readl(... QUEUE_NUM_MAX);
410 if (num == 0) {
411     err = -ENOENT;
412     goto error_new_virtqueue;
}
```

接著我在 semu 的 `virtio-fs.c` 暫時加了最窄的 MMIO trace，只印:

- `QueueSel`
- `QueueReady`
- `QueueNumMax`

再跑:

```bash
make check ENABLE_INPUT_DEBUG=1 SMP=4
```

關鍵輸出:

```text
vfs-mmio: write QueueSel=0
vfs-mmio: read QueueReady q=0 value=0
vfs-mmio: read QueueNumMax q=0 value=1024
vfs-mmio: write QueueReady q=0 value=1

vfs-mmio: write QueueSel=1
vfs-mmio: read QueueReady q=1 value=0
vfs-mmio: read QueueNumMax q=1 value=1024
vfs-mmio: write QueueReady q=1 value=1

vfs-mmio: write QueueSel=2
vfs-mmio: read QueueReady q=2 value=0
vfs-mmio: read QueueNumMax q=2 value=1024
vfs-mmio: write QueueReady q=2 value=1

vfs-mmio: invalid QueueSel=3 max=3 current=2
vfs-mmio: read QueueReady q=2 value=1
```

這段輸出的意義很直接:

1. guest 真的在 setup 第 4 個 queue，也就是 `QueueSel=3`
2. semu 端卻只接受 `QueueSel < ARRAY_SIZE(vfs->queues)`，而目前 host `virtio_fs_state_t` 是 `queues[3]`
3. 因為 `QueueSel=3` 被判 invalid，host 沒有把 `QueueSel` 更新成 3，仍然停在前一個 queue 2
4. guest 下一步讀 `QueueReady` 時，實際讀到的是 **queue 2 的 `ready=1`**
5. 這正好命中 guest `virtio_mmio` 的:

```text
if (readl(... QUEUE_READY)) err = -ENOENT;
```

所以 `virtiofs virtio4: probe ... error -2` 的直接來源，現在也已經收斂成一個很明確的 host bug:

- semu `virtio-fs` 回報的 `num_request_queues`
- 和它實際支援的 queue array 大小
- 兩者不一致

具體對照是:

```text
# host
device.h: virtio_fs_state_t queues[3]
virtio-fs.c: PRIV(vfs)->num_request_queues = 3

# guest
fs->nvqs = 1 + num_request_queues = 4
```

也就是說，host 現在宣告了:

- `3` 個 request queue

但實際上只準備了:

- `3` 個 total queue slots

guest 卻依規則建立:

- `1` 個 hiprio queue
- `3` 個 request queue
- 合計 `4` 個 queue

這個 off-by-one / 語意不一致，正是 `virtiofs` fail probe 的第一張骨牌。

### 更新後的整體因果鏈

到這一步，整條鏈已經可以寫得很完整:

1. semu `virtio-fs` 回報 `num_request_queues = 3`，但 host state 只支援 `queues[3]`
2. guest 依規則建立 `4` 個 queue，做到 `QueueSel=3` 時撞到 host invalid path
3. guest 因為讀到前一個 queue 的 `QueueReady=1`，在 `virtio_mmio` 拿到 `-ENOENT`
4. guest `virtio_fs_probe()` fail
5. guest `virtio_fs_setup_vqs()` 先 free `mq_map` / `vqs`
6. guest `virtio_fs_probe()` 後續 `kobject_put(&fs->kobj)`，又進 `virtio_fs_ktype_release()` 再 free 一次
7. `mq_map` 被 double-free，形成 self-linked freelist
8. 同一個 object 之後被 `kstrdup_const(__kernfs_new_node)` 重用成 16-byte name buffer
9. `.vir...` 被 memcpy 進去後，SLUB 再把字串當 freelist pointer 消費，最後在 `__update_cpu_freelist_fast` commit
10. 後面 `virtio_input` probe 再做一次 `kmalloc` 時，就踩到 `0x7269762e / 0x72697636` panic

這代表：

- `virtio-input` 不是 direct corrupter
- `virtiofs` 也不是「莫名其妙 fail」；它的 fail 現在有了非常具體的 host-side 原因
- 目前這個 panic 最接近「host `virtio-fs` queue count bug 觸發 guest `virtio_fs` driver 的 double-free，再由 `vinput` 後續 allocation 把結果炸出來」

這也可以順手解釋先前 `master` 的對照:

- `master` 的 boot log 裡其實也看得到 `virtiofs ... failed with error -2`
- 但 `master` 沒有現在這兩個 `virtio-input` device，所以少了後面那串很早就會發生的小型 allocation / sysfs name duplication
- 因此同一個已 double-free 的 `mq_map`，在 `master` 上不一定會立刻被重用成會爆炸的 shape
- 到 `vinput-pr` 這條線，double-free 後很快就接上 `virtio_input` probe 與 `kernfs name` 的 allocation，才把問題穩定放大成早期 panic
