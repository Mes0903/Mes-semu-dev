# 階段 4：中斷系統

> 理解 PLIC 與 ACLINT 中斷控制器（預估 1-2 天）

---

## 📋 本階段目標

- ✅ 理解 PLIC 平台中斷控制器的工作原理
- ✅ 掌握 ACLINT（MTIMER、MSWI、SSWI）三個組件
- ✅ 理解中斷路由與處理流程
- ✅ 掌握中斷優先級與遮罩機制

---

## 📚 閱讀清單

### 必讀文檔

- [RISC-V PLIC Spec](https://github.com/riscv/riscv-plic-spec)
- [RISC-V ACLINT Spec](https://github.com/riscv/riscv-aclint)

### 必讀代碼

- [plic.c](../plic.c) - 完整閱讀（128 行）
- [aclint.c](../aclint.c) - 完整閱讀（227 行）
- [device.h](../device.h):29-314 - 中斷控制器結構定義

---

## 🎯 中斷系統架構

### 整體中斷流程

```
外設設備（UART、VirtIO）
    │
    ├─ 觸發中斷（設置中斷線）
    │
    ▼
┌─────────────────────────────────┐
│  PLIC (Platform-Level IC)       │
│  ├─ active（輸入線狀態）        │
│  ├─ ip（待處理中斷）            │
│  └─ ie[context]（使能遮罩）     │
└──────────────┬──────────────────┘
               │ 路由到目標 hart
    ┌──────────┼──────────┐
    │          │          │
    ▼          ▼          ▼
┌────────┐ ┌────────┐ ┌────────┐
│ Hart 0 │ │ Hart 1 │ │ Hart N │
│ sip    │ │ sip    │ │ sip    │
│ [SEI]  │ │ [SEI]  │ │ [SEI]  │
└────────┘ └────────┘ └────────┘
    │          │          │
    └──────────┴──────────┘
               │ if (sip & sie & sstatus_sie)
               ▼
         hart_trap()
         （進入中斷處理）
```

### 中斷類型

```c
S-mode 中斷類型（sip 位元）：
┌─────┬────────────────────────────┐
│ Bit │     中斷類型               │
├─────┼────────────────────────────┤
│  1  │ SSI - 軟體中斷（IPI）      │
│  5  │ STI - 計時器中斷           │
│  9  │ SEI - 外部中斷（PLIC）     │
└─────┴────────────────────────────┘

對應位元：
  RV_INT_SSI_BIT = (1 << 1)  = 0x002
  RV_INT_STI_BIT = (1 << 5)  = 0x020
  RV_INT_SEI_BIT = (1 << 9)  = 0x200
```

---

## 🔌 PLIC（平台中斷控制器）

### 1. PLIC 設計理念

SEMU 的 PLIC 實現極簡化：
- **32 個中斷源**（IRQ 1-31，IRQ 0 保留）
- **無優先級**（所有中斷優先級為 1）
- **32 個上下文**（支援 32 個 harts）

### 2. PLIC 數據結構

**位置**：[device.h](../device.h):29-35

```c
typedef struct {
    uint32_t masked;    // 已遮罩的中斷（正在處理）
    uint32_t ip;        // 中斷待處理位（Interrupt Pending）
    uint32_t ie[32];    // 中斷使能（Interrupt Enable，每個上下文）
    uint32_t active;    // 輸入中斷線狀態（外設設置）
} plic_state_t;
```

### 3. 中斷路由

**位置**：[plic.c](../plic.c):7-22

```c
void plic_update_interrupts(vm_t *vm, plic_state_t *plic)
{
    // ═══════════════════════════════════════════
    // 步驟 1：更新待處理中斷
    // ═══════════════════════════════════════════
    // ip |= 新觸發的中斷（active 且未 masked）
    plic->ip |= plic->active & ~plic->masked;
    // 標記為已遮罩
    plic->masked |= plic->active;

    // ═══════════════════════════════════════════
    // 步驟 2：路由到各個 hart
    // ═══════════════════════════════════════════
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        // 檢查是否有該 hart 使能的中斷
        if (plic->ip & plic->ie[i]) {
            // 設置外部中斷位
            vm->hart[i]->sip |= RV_INT_SEI_BIT;
            // 清除 WFI 標記（喚醒 hart）
            vm->hart[i]->in_wfi = false;
        } else {
            // 清除外部中斷位
            vm->hart[i]->sip &= ~RV_INT_SEI_BIT;
        }
    }
}
```

**工作流程**：

```
外設觸發中斷：
  plic.active |= (1 << IRQ_UART)  // 設置中斷線
    ↓
plic_update_interrupts()
    ↓
  更新 ip：
    plic.ip |= plic.active & ~plic.masked
    plic.masked |= plic.active
    ↓
  檢查每個 hart：
    for i in harts:
      if (plic.ip & plic.ie[i]):  // 有使能的中斷
        hart[i].sip |= SEI_BIT     // 設置外部中斷
    ↓
vm_step() 檢查中斷：
  if (sip & sie & sstatus_sie):
    hart_trap()
    ↓
Linux 中斷處理：
  讀取 PLIC claim 暫存器 → 獲取中斷號
  呼叫中斷處理程序
  寫入 PLIC completion 暫存器 → 完成中斷
    ↓
  plic.masked &= ~(1 << irq)  // 取消遮罩
```

### 4. PLIC 暫存器

**位置**：[plic.c](../plic.c):24-85

#### 暫存器映射

```
PLIC 基地址：0xF0000000

寄存器佈局：
0x000000: Source 0 Priority（保留）
0x000004: Source 1 Priority  ──┐
  ...                           ├─ 源優先級（SEMU 固定為 1）
0x00007C: Source 31 Priority ──┘

0x000400: Pending Bits        ── ip（待處理中斷）

0x000800: Enable Bits (Context 0)  ──┐
0x000804: Enable Bits (Context 1)    ├─ ie[context]
  ...                                 │  （每個 hart 的使能）
0x000880: Enable Bits (Context 31) ──┘

0x080000: Priority Threshold (Context 0) ──┐
  ...                                       ├─ 閾值（SEMU 固定為 0）
0x080078: Priority Threshold (Context 31)──┘

0x080004: Claim/Complete (Context 0) ──┐
  ...                                   ├─ Claim/Complete
0x08007C: Claim/Complete (Context 31)──┘
```

#### Claim 操作（讀取）

```c
case 0x80001:  // Claim 暫存器
    *value = 0;
    uint32_t candidates = plic->ip & plic->ie[context];

    if (candidates) {
        // 找到最高優先級中斷（這裡是最低位元）
        *value = ilog2(candidates);
        // 清除 ip 位元（開始處理）
        plic->ip &= ~(1 << (*value));
    }
    return true;
```

**Linux 使用**：
```c
// Linux PLIC 驅動程式
void handle_plic_irq(void) {
    // 讀取 claim 暫存器
    uint32_t irq = readl(PLIC_BASE + PLIC_CLAIM);

    if (irq) {
        // 呼叫中斷處理程序
        generic_handle_irq(irq);

        // 寫入 completion 暫存器
        writel(irq, PLIC_BASE + PLIC_COMPLETION);
    }
}
```

#### Complete 操作（寫入）

```c
case 0x80001:  // Completion 暫存器
    // 取消遮罩（允許再次觸發）
    if (plic->ie[context] & (1 << value))
        plic->masked &= ~(1 << value);
    return true;
```

### 5. IRQ 分配

**位置**：[device.h](../device.h):50-138

```c
#define IRQ_UART  1    // UART 串口
#define IRQ_VNET  2    // VirtIO-Net 網路
#define IRQ_VBLK  3    // VirtIO-Blk 磁碟
#define IRQ_VRNG  4    // VirtIO-RNG 隨機數
#define IRQ_VSND  5    // VirtIO-Sound 音效
#define IRQ_VFS   6    // VirtIO-FS 文件系統
```

**設備觸發中斷**：
```c
// 範例：UART 觸發中斷
void uart_trigger_interrupt(emu_state_t *state) {
    state->plic.active |= IRQ_UART_BIT;  // 設置中斷線
    plic_update_interrupts(&state->vm, &state->plic);
}
```

---

## ⏰ ACLINT（核心本地中斷）

ACLINT 提供三個組件：
1. **MTIMER** - 64 位元計時器
2. **MSWI** - M-mode 軟體中斷
3. **SSWI** - S-mode 軟體中斷

### 1. MTIMER（計時器）

**位置**：[device.h](../device.h):229-248

```c
typedef struct {
    uint64_t *mtimecmp;  // 每個 hart 的比較值陣列
    semu_timer_t mtime;  // 當前時間（64 位元）
} mtimer_state_t;
```

**記憶體映射**：
```
基地址：0x0D000000

0x0000: mtime（低 32 位）
0x0004: mtime（高 32 位）
0x4000: mtimecmp[0]（低 32 位）
0x4004: mtimecmp[0]（高 32 位）
0x4008: mtimecmp[1]（低 32 位）
0x400C: mtimecmp[1]（高 32 位）
...
```

**工作原理**：

```c
// 檢查計時器中斷（主循環）
void aclint_mtimer_update_interrupts(hart_t *hart, mtimer_state_t *mtimer)
{
    uint64_t mtime = get_current_time();
    uint64_t mtimecmp = mtimer->mtimecmp[hart->mhartid];

    if (mtime >= mtimecmp) {
        // 觸發計時器中斷
        hart->sip |= RV_INT_STI_BIT;
    } else {
        // 清除計時器中斷
        hart->sip &= ~RV_INT_STI_BIT;
    }
}
```

**Linux 使用**：
```c
// Linux 計時器設置
void riscv_timer_set(uint64_t delta_ns) {
    uint64_t ticks = delta_ns * TIMEBASE_FREQ / 1000000000;
    uint64_t target = read_time() + ticks;

    // SBI ecall SET_TIMER
    sbi_set_timer(target);
}

// SBI 實現（在 SEMU 中）
void sbi_set_timer(uint64_t time_value) {
    mtimer.mtimecmp[hartid] = time_value;
    hart->sip &= ~RV_INT_STI_BIT;  // 清除舊中斷
}
```

### 2. MSWI（M-mode 軟體中斷）

**位置**：[device.h](../device.h):263-275

```c
typedef struct {
    uint32_t *msip;  // 每個 hart 的 MSIP 暫存器陣列
} mswi_state_t;
```

**記憶體映射**：
```
基地址：0x0C000000

0x0000: msip[0]
0x0004: msip[1]
0x0008: msip[2]
...
```

**工作原理**：
```c
void aclint_mswi_update_interrupts(hart_t *hart, mswi_state_t *mswi)
{
    if (mswi->msip[hart->mhartid]) {
        // 設置軟體中斷（通過 SSWI 映射到 S-mode）
        hart->sip |= RV_INT_SSI_BIT;
    }
}
```

### 3. SSWI（S-mode 軟體中斷）

**位置**：[device.h](../device.h):290-302

```c
typedef struct {
    uint32_t *ssip;  // 每個 hart 的 SSIP 暫存器陣列
} sswi_state_t;
```

**記憶體映射**：
```
基地址：0x0E000000

0x0000: ssip[0]
0x0004: ssip[1]
0x0008: ssip[2]
...
```

**SSWI vs MSWI**：

```
MSWI（SBI 使用）：
  - SBI IPI 擴展寫入 MSWI
  - M-mode 特權
  - 映射到 hart.sip[SSI]

SSWI（OS 直接使用）：
  - Linux 可以直接寫入 SSWI
  - S-mode 特權
  - 也映射到 hart.sip[SSI]

實際上，SEMU 的 SBI IPI 實現直接使用 SSWI：
  sbi_send_ipi() → data->sswi.ssip[i] = 1
```

---

## 🔄 完整中斷處理流程

### 外部中斷（UART 範例）

```
1. UART 接收到數據
   ├─ 設置 LSR.DR 位元（Data Ready）
   └─ u8250_update_interrupts()
       └─ state->plic.active |= IRQ_UART_BIT

2. PLIC 處理
   ├─ plic_update_interrupts()
   │   ├─ plic.ip |= active & ~masked
   │   └─ hart->sip |= RV_INT_SEI_BIT
   └─ hart->in_wfi = false（喚醒 hart）

3. vm_step() 檢查中斷
   if ((sstatus_sie || !s_mode) && (sip & sie))
       ├─ applicable = sip & sie
       ├─ idx = ilog2(applicable)  // idx = 9（SEI）
       └─ hart_trap()

4. hart_trap() 陷阱處理
   ├─ sepc = current_pc
   ├─ scause = 0x80000009（中斷 + SEI）
   ├─ stval = 0
   ├─ sstatus_spp = s_mode
   ├─ sstatus_spie = sstatus_sie
   ├─ s_mode = true
   ├─ sstatus_sie = false
   └─ pc = stvec_addr

5. Linux 中斷處理
   ├─ 保存上下文
   ├─ 讀取 PLIC claim → irq = 1（UART）
   ├─ generic_handle_irq(1)
   │   └─ uart_interrupt_handler()
   │       ├─ 讀取 UART 數據
   │       └─ 喚醒等待進程
   ├─ 寫入 PLIC completion(1)
   │   └─ plic.masked &= ~IRQ_UART_BIT
   └─ SRET 返回

6. SRET 恢復
   ├─ pc = sepc
   ├─ s_mode = sstatus_spp
   └─ sstatus_sie = sstatus_spie
```

### 計時器中斷

```
1. Linux 設置計時器
   sbi_set_timer(target_time)
   └─ mtimer.mtimecmp[hartid] = target_time

2. 模擬器主循環
   aclint_mtimer_update_interrupts()
   if (mtime >= mtimecmp[hartid])
       hart->sip |= RV_INT_STI_BIT

3. vm_step() 檢查中斷
   if (sip & sie & sstatus_sie)
       ├─ idx = 5（STI）
       └─ hart_trap()

4. Linux 計時器處理
   ├─ 更新 jiffies
   ├─ 檢查定時器隊列
   ├─ 進程調度（如需要）
   └─ 設置下一個計時器
       └─ sbi_set_timer(next_time)
```

### IPI（處理器間中斷）

```
1. Hart 0 發送 IPI 給 Hart 1
   sbi_send_ipi(hart_mask=0b10)
   └─ data->sswi.ssip[1] = 1

2. Hart 1 主循環
   aclint_sswi_update_interrupts()
   if (ssip[1])
       hart->sip |= RV_INT_SSI_BIT

3. Hart 1 處理 IPI
   ├─ idx = 1（SSI）
   ├─ hart_trap()
   └─ Linux IPI handler
       ├─ 執行請求的操作（TLB flush、函數呼叫等）
       └─ 清除 ssip[1] = 0
```

---

## 🎓 實踐練習

### 練習 1：觀察 PLIC 中斷

```bash
(gdb) break plic_update_interrupts
(gdb) continue

# UART 接收數據時觸發
(gdb) print/x plic->active
(gdb) print/x plic->ip
(gdb) print/x plic->ie[0]
(gdb) step
(gdb) print/x vm->hart[0]->sip
```

### 練習 2：追蹤計時器中斷

```bash
(gdb) break aclint_mtimer_update_interrupts
(gdb) continue

(gdb) print mtimer->mtime
(gdb) print mtimer->mtimecmp[0]
(gdb) step
(gdb) print/x hart->sip
```

### 練習 3：測試 IPI

```bash
# 在 Linux 中（4 核心）
echo 1 > /sys/devices/system/cpu/cpu1/online

# GDB 追蹤
(gdb) break handle_sbi_ecall_IPI
(gdb) continue
(gdb) print/x hart_mask
```

---

## 📊 知識檢查點

1. **Q**: PLIC 支援多少個中斷源？
   **A**: 32 個（IRQ 1-31）

2. **Q**: ACLINT 的三個組件是什麼？
   **A**: MTIMER、MSWI、SSWI

3. **Q**: sip 的哪些位元表示中斷？
   **A**: bit 1 (SSI), bit 5 (STI), bit 9 (SEI)

4. **Q**: PLIC claim 操作做了什麼？
   **A**: 返回最高優先級中斷號並清除 ip 位元

5. **Q**: 為何需要 masked 欄位？
   **A**: 防止同一中斷被多次處理

---

## 🎯 下一階段預告

**階段 5** 將深入研究外設設備實現。

繼續閱讀：[階段 5：外設設備](./stage5-peripherals.md)

---

**文檔版本**：v1.0
**最後更新**：2025-01-10
**預估學習時間**：1-2 天
