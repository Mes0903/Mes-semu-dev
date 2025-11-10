# 階段 3：系統服務層

> 深入理解 SBI 實現、多核心管理與協程式調度（預估 2-3 天）

---

## 📋 本階段目標

- ✅ 掌握 SBI 0.2 標準的完整實現
- ✅ 理解 HSM（Hart State Management）多核心管理
- ✅ 理解 IPI（Inter-Processor Interrupt）處理器間中斷
- ✅ 理解 RFENCE（Remote Fence）遠程柵欄機制
- ✅ 深入理解協程式 SMP 調度系統
- ✅ 掌握事件驅動等待機制

---

## 📚 閱讀清單

### 必讀文檔

- [SBI Specification 0.2](https://github.com/riscv-non-isa/riscv-sbi-doc) - SBI 標準規範
- [專案架構概覽](./project-overview.md) - 回顧系統架構
- [階段 2：CPU 核心機制](./stage2-cpu-core.md) - 回顧異常處理

### 必讀代碼

**SBI 實現**：
- [main.c](../main.c):533-563 - `handle_sbi_ecall()` 主入口
- [main.c](../main.c):503-528 - `handle_sbi_ecall_BASE()` 基礎擴展
- [main.c](../main.c):317-348 - `handle_sbi_ecall_TIMER()` 計時器擴展
- [main.c](../main.c):350-423 - `handle_sbi_ecall_HSM()` 多核心管理
- [main.c](../main.c):425-446 - `handle_sbi_ecall_IPI()` 處理器間中斷
- [main.c](../main.c):448-497 - `handle_sbi_ecall_RFENCE()` 遠程柵欄

**協程系統**：
- [coro.c](../coro.c):0-200 - 協程結構與平台檢測
- [coro.c](../coro.c):200-350 - 協程初始化與切換
- [coro.c](../coro.c):350-500 - 協程調度與喚醒
- [coro.c](../coro.c):500-615 - 事件驅動等待機制
- [coro.h](../coro.h) - 協程 API 定義

---

## 🎯 核心概念

### 1. SBI（Supervisor Binary Interface）簡介

**SBI 是什麼？**

SBI 是 RISC-V 的**監督模式二進制介面**，定義了 S-mode（Supervisor）與 M-mode（Machine）之間的標準通信協議。

```
┌─────────────────────────────────────────┐
│      User Applications (U-mode)         │
└──────────────┬──────────────────────────┘
               │ syscall
┌──────────────▼──────────────────────────┐
│      Linux Kernel (S-mode)              │
│  - Process Management                   │
│  - Memory Management                    │
│  - Device Drivers                       │
└──────────────┬──────────────────────────┘
               │ SBI ecall (S → M)
┌──────────────▼──────────────────────────┐
│    SBI Implementation (M-mode)          │
│    (SEMU 模擬器充當這一層)              │
│  - Timer Management                     │
│  - Hart Management (多核心)             │
│  - IPI (處理器間中斷)                   │
│  - RFENCE (TLB 同步)                    │
└─────────────────────────────────────────┘
```

**為何需要 SBI？**

1. **抽象硬體差異**：不同 RISC-V 實現可能有不同的 M-mode 設備，SBI 提供統一介面
2. **權限分離**：S-mode 無法直接訪問某些硬體（如計時器、IPI），需通過 SBI
3. **可移植性**：Linux 只需實現 SBI 客戶端，無需關心底層實現

### 2. SBI 呼叫機制

#### ecall 指令

S-mode 通過 `ecall` 指令呼叫 SBI：

```assembly
# Linux 呼叫 SBI
li a7, 0x54494D45      # SBI_EID_TIMER（擴展 ID）
li a6, 0               # SBI_TIMER__SET_TIMER（功能 ID）
mv a0, <time_value>    # 參數
ecall                  # 觸發 S-mode 異常
# 返回後，a0 包含錯誤碼，a1 包含返回值
```

#### 參數傳遞約定

```
暫存器約定：
  a7 (x17) - Extension ID (EID)
  a6 (x16) - Function ID (FID)
  a0-a5 (x10-x15) - 參數

返回值：
  a0 (x10) - error (錯誤碼)
  a1 (x11) - value (返回值)
```

---

## 🔧 SBI 擴展實現

### 1. BASE 擴展（基礎功能）

**位置**：[main.c](../main.c):503-528

```c
static inline sbi_ret_t handle_sbi_ecall_BASE(hart_t *hart, int32_t fid)
{
    switch (fid) {
    // ═══════════════════════════════════════════
    // 查詢 SBI 實現信息
    // ═══════════════════════════════════════════
    case SBI_BASE__GET_SBI_IMPL_ID:
        // 返回實現 ID（SEMU 的自定義 ID）
        return (sbi_ret_t){SBI_SUCCESS, SBI_IMPL_ID};

    case SBI_BASE__GET_SBI_IMPL_VERSION:
        // 返回實現版本
        return (sbi_ret_t){SBI_SUCCESS, SBI_IMPL_VERSION};

    case SBI_BASE__GET_SBI_SPEC_VERSION:
        // 返回 SBI 規範版本（2.0）
        return (sbi_ret_t){SBI_SUCCESS, (2 << 24) | 0};

    // ═══════════════════════════════════════════
    // 查詢機器信息
    // ═══════════════════════════════════════════
    case SBI_BASE__GET_MVENDORID:
        // 返回廠商 ID
        return (sbi_ret_t){SBI_SUCCESS, RV_MVENDORID};

    case SBI_BASE__GET_MARCHID:
        // 返回架構 ID
        return (sbi_ret_t){SBI_SUCCESS, RV_MARCHID};

    case SBI_BASE__GET_MIMPID:
        // 返回實現 ID
        return (sbi_ret_t){SBI_SUCCESS, RV_MIMPID};

    // ═══════════════════════════════════════════
    // 功能探測
    // ═══════════════════════════════════════════
    case SBI_BASE__PROBE_EXTENSION: {
        int32_t eid = (int32_t) hart->x_regs[RV_R_A0];

        // 檢查擴展是否可用
        bool available = eid == SBI_EID_BASE ||
                         eid == SBI_EID_TIMER ||
                         eid == SBI_EID_RST ||
                         eid == SBI_EID_HSM ||
                         eid == SBI_EID_IPI ||
                         eid == SBI_EID_RFENCE;

        return (sbi_ret_t){SBI_SUCCESS, available};
    }

    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}
```

**BASE 擴展的作用**：

Linux 在啟動時會：
1. 呼叫 `GET_SBI_SPEC_VERSION` 確認 SBI 版本
2. 呼叫 `PROBE_EXTENSION` 檢測可用擴展
3. 根據可用擴展啟用相應功能

---

### 2. TIMER 擴展（計時器管理）

**位置**：[main.c](../main.c):317-348

```c
static inline sbi_ret_t handle_sbi_ecall_TIMER(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);

    switch (fid) {
    case SBI_TIMER__SET_TIMER: {
        // ═══════════════════════════════════════════
        // 設置計時器中斷
        // ═══════════════════════════════════════════

        // 讀取 64 位元時間值（a0 = 低 32 位，a1 = 高 32 位）
        uint64_t time_value = ((uint64_t) hart->x_regs[RV_R_A1] << 32) |
                              hart->x_regs[RV_R_A0];

        // 設置當前 hart 的 MTIMECMP
        data->mtimer.mtimecmp[hart->mhartid] = time_value;

        // 清除計時器中斷（會在 mtime >= mtimecmp 時重新觸發）
        hart->sip &= ~RV_INT_STI_BIT;

        return (sbi_ret_t){SBI_SUCCESS, 0};
    }

    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}
```

**計時器工作原理**：

```
Linux 想在 1ms 後觸發中斷：
  │
  ├─ 讀取當前時間（通過 CSR TIME）
  │    current_time = 10000000  (10MHz = 每 0.1μs 一個 tick)
  │
  ├─ 計算目標時間
  │    target_time = current_time + 10000  (1ms = 10000 ticks)
  │
  ├─ SBI ecall：SET_TIMER(target_time)
  │    a0 = target_time & 0xFFFFFFFF
  │    a1 = target_time >> 32
  │    ecall
  │
  └─ SBI 設置 mtimecmp[hartid] = target_time

模擬器主循環：
  while (running) {
      vm_step();

      // 檢查計時器
      if (mtime >= mtimecmp[hartid])
          hart->sip |= RV_INT_STI_BIT;  // 觸發計時器中斷

      // 檢查中斷
      if (sip & sie & sstatus_sie)
          hart_trap();  // 進入中斷處理
  }
```

**時間基準**：
```c
// 設備樹中定義
timebase-frequency = <10000000>;  // 10MHz

// Linux 根據 timebase 計算延遲
void sbi_set_timer(uint64_t ns) {
    uint64_t ticks = ns * timebase / 1000000000;
    uint64_t target = read_time() + ticks;
    sbi_ecall(SBI_EID_TIMER, SBI_TIMER__SET_TIMER,
              target & 0xFFFFFFFF, target >> 32, 0, 0, 0, 0);
}
```

---

### 3. HSM 擴展（多核心管理）

**位置**：[main.c](../main.c):350-423

#### HSM 狀態機

```
                    ┌──────────────┐
                    │   STOPPED    │ ← 初始狀態（除 hart 0）
                    └──────┬───────┘
                           │ HART_START
                    ┌──────▼───────┐
         ┌──────────│   STARTED    │◄─────────┐
         │          └──────┬───────┘          │
         │                 │                   │
         │ HART_SUSPEND    │ HART_STOP         │ Resume
         │                 │                   │
    ┌────▼────┐       ┌────▼────┐       ┌─────┴──────┐
    │SUSPENDED│       │ STOPPED │       │RESUME_PEND.│
    └─────────┘       └─────────┘       └────────────┘
```

#### HART_START 實現

```c
case SBI_HSM__HART_START: {
    // ═══════════════════════════════════════════
    // 啟動指定的 hart
    // ═══════════════════════════════════════════

    // 讀取參數
    uint32_t hartid = hart->x_regs[RV_R_A0];      // 目標 hart ID
    uint32_t start_addr = hart->x_regs[RV_R_A1];  // 起始地址
    uint32_t opaque = hart->x_regs[RV_R_A2];      // 不透明參數

    // 驗證 hart ID
    if (hartid >= vm->n_hart)
        return (sbi_ret_t){SBI_ERR_INVALID_PARAM, 0};

    hart_t *target_hart = vm->hart[hartid];

    // 檢查當前狀態
    if (target_hart->hsm_status == SBI_HSM_STATE_STARTED)
        return (sbi_ret_t){SBI_ERR_ALREADY_AVAILABLE, 0};

    // ═══════════════════════════════════════════
    // 設置 hart 初始狀態
    // ═══════════════════════════════════════════
    target_hart->pc = start_addr;             // 設置 PC
    target_hart->x_regs[RV_R_A0] = hartid;    // a0 = hartid
    target_hart->x_regs[RV_R_A1] = opaque;    // a1 = opaque
    target_hart->s_mode = true;               // S-mode
    target_hart->hsm_status = SBI_HSM_STATE_STARTED;

    return (sbi_ret_t){SBI_SUCCESS, 0};
}
```

**Linux 啟動次級核心**：

```c
// Linux 內核代碼（簡化）
void smp_boot_secondary(int cpu) {
    // 準備次級核心的堆疊和上下文
    unsigned long stack = alloc_stack(cpu);
    unsigned long entry = (unsigned long)secondary_start_kernel;

    // SBI 呼叫啟動 hart
    struct sbiret ret = sbi_ecall(SBI_EID_HSM,
                                   SBI_HSM__HART_START,
                                   cpu,     // hartid
                                   entry,   // start_addr
                                   stack,   // opaque
                                   0, 0, 0);

    if (ret.error != 0)
        pr_err("Failed to boot CPU %d\n", cpu);
}

// 次級核心入口
void secondary_start_kernel(unsigned long hartid, unsigned long stack) {
    // a0 = hartid, a1 = stack（來自 opaque）
    setup_cpu(hartid);
    local_irq_enable();
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}
```

#### HART_STOP 實現

```c
case SBI_HSM__HART_STOP: {
    // ═══════════════════════════════════════════
    // 停止當前 hart
    // ═══════════════════════════════════════════

    // 設置為 STOPPED 狀態
    hart->hsm_status = SBI_HSM_STATE_STOPPED;

    // 不會返回（hart 進入停止狀態）
    return (sbi_ret_t){SBI_SUCCESS, 0};
}
```

**注意**：`HART_STOP` 不會返回到呼叫者，hart 會進入 STOPPED 狀態直到被 `HART_START` 重新啟動。

#### HART_SUSPEND 實現

```c
case SBI_HSM__HART_SUSPEND: {
    // ═══════════════════════════════════════════
    // 掛起當前 hart
    // ═══════════════════════════════════════════

    uint32_t suspend_type = hart->x_regs[RV_R_A0];
    uint32_t resume_addr = hart->x_regs[RV_R_A1];
    uint32_t opaque = hart->x_regs[RV_R_A2];

    hart->hsm_status = SBI_HSM_STATE_SUSPENDED;

    if (suspend_type == 0x00000000) {
        // ═══════════════════════════════════════════
        // Retentive suspend（保持狀態掛起）
        // ═══════════════════════════════════════════
        // 恢復時返回到 ecall 之後
        hart->hsm_resume_is_ret = true;
        hart->hsm_resume_pc = hart->pc;  // 保存當前 PC
    } else if (suspend_type == 0x80000000) {
        // ═══════════════════════════════════════════
        // Non-retentive suspend（不保持狀態掛起）
        // ═══════════════════════════════════════════
        // 恢復時跳轉到新地址（類似重啟）
        hart->hsm_resume_is_ret = false;
        hart->hsm_resume_pc = resume_addr;
        hart->hsm_resume_opaque = opaque;
    }

    return (sbi_ret_t){SBI_SUCCESS, 0};
}
```

**兩種掛起模式**：

```
Retentive suspend (suspend_type=0)：
  - 保持所有暫存器狀態
  - 恢復時從 ecall 之後繼續執行
  - 用於輕度睡眠（如 WFI）

Non-retentive suspend (suspend_type=0x80000000)：
  - 不保持暫存器（除 hartid 和 opaque）
  - 恢復時跳轉到 resume_addr
  - 用於深度睡眠或 CPU 熱插拔
```

#### HART_GET_STATUS 實現

```c
case SBI_HSM__HART_GET_STATUS: {
    uint32_t hartid = hart->x_regs[RV_R_A0];

    // 返回指定 hart 的狀態
    return (sbi_ret_t){SBI_SUCCESS, vm->hart[hartid]->hsm_status};
}
```

---

### 4. IPI 擴展（處理器間中斷）

**位置**：[main.c](../main.c):425-446

```c
static inline sbi_ret_t handle_sbi_ecall_IPI(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);

    switch (fid) {
    case SBI_IPI__SEND_IPI: {
        // ═══════════════════════════════════════════
        // 發送 IPI 給指定的 harts
        // ═══════════════════════════════════════════

        // 讀取 hart mask（64 位元，支援最多 64 個 harts）
        uint64_t hart_mask = (uint64_t) hart->x_regs[RV_R_A0];
        uint64_t hart_mask_base = (uint64_t) hart->x_regs[RV_R_A1];

        if (hart_mask_base == 0xFFFFFFFFFFFFFFFF) {
            // ═══════════════════════════════════════════
            // 特殊值：發送給所有 harts
            // ═══════════════════════════════════════════
            for (uint32_t i = 0; i < hart->vm->n_hart; i++)
                data->sswi.ssip[i] = 1;
        } else {
            // ═══════════════════════════════════════════
            // 根據 mask 發送給指定 harts
            // ═══════════════════════════════════════════
            for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++) {
                if (hart_mask & 1)
                    data->sswi.ssip[i] = 1;
            }
        }

        return (sbi_ret_t){SBI_SUCCESS, 0};
    }

    default:
        return (sbi_ret_t){SBI_ERR_FAILED, 0};
    }
}
```

**Hart Mask 編碼**：

```
hart_mask_base 和 hart_mask 組合表示要發送 IPI 的 harts：

範例 1：發送給 hart 0, 2, 5
  hart_mask_base = 0
  hart_mask = 0b00100101

  處理：
    i=0: mask=0b00100101, bit 0=1 → ssip[0]=1
    i=1: mask=0b00010010, bit 0=0 → 跳過
    i=2: mask=0b00001001, bit 0=1 → ssip[2]=1
    i=3: mask=0b00000100, bit 0=0 → 跳過
    i=4: mask=0b00000010, bit 0=0 → 跳過
    i=5: mask=0b00000001, bit 0=1 → ssip[5]=1

範例 2：發送給 hart 64-67
  hart_mask_base = 64
  hart_mask = 0b1111

  處理：
    i=64: mask=0b1111, bit 0=1 → ssip[64]=1
    i=65: mask=0b0111, bit 0=1 → ssip[65]=1
    i=66: mask=0b0011, bit 0=1 → ssip[66]=1
    i=67: mask=0b0001, bit 0=1 → ssip[67]=1

範例 3：發送給所有 harts
  hart_mask_base = 0xFFFFFFFFFFFFFFFF
  → 直接設置所有 ssip[i]=1
```

**IPI 完整流程**：

```
Hart 0 想喚醒 Hart 1：
  │
  ├─ Linux 呼叫 smp_send_ipi(1)
  │
  ├─ SBI ecall：SEND_IPI
  │    a0 = 0b10  (hart_mask)
  │    a1 = 0     (hart_mask_base)
  │    ecall
  │
  ├─ SBI 設置 sswi.ssip[1] = 1
  │
  └─ 模擬器主循環：
      aclint_sswi_update_interrupts(hart1)
        if (ssip[1] == 1)
            hart1->sip |= RV_INT_SSI_BIT

      vm_step(hart1)
        if (sip & sie & sstatus_sie)
            hart_trap()  // Hart 1 進入中斷處理

      Linux IPI handler:
        handle_IPI()
          - 執行請求的操作
          - 清除 sip[SSI]（寫入 sip CSR）
```

---

### 5. RFENCE 擴展（遠程柵欄）

**位置**：[main.c](../main.c):448-497

```c
static inline sbi_ret_t handle_sbi_ecall_RFENCE(hart_t *hart, int32_t fid)
{
    uint64_t hart_mask, hart_mask_base;
    uint32_t start_addr, size;

    switch (fid) {
    case SBI_RFENCE__I:
        // ═══════════════════════════════════════════
        // RFENCE.I：失效指令快取
        // ═══════════════════════════════════════════
        // 在解釋器模式下被忽略（無指令快取）
        return (sbi_ret_t){SBI_SUCCESS, 0};

    case SBI_RFENCE__VMA:
    case SBI_RFENCE__VMA_ASID: {
        // ═══════════════════════════════════════════
        // RFENCE.VMA：失效 TLB
        // ═══════════════════════════════════════════

        // 讀取參數
        hart_mask = (uint64_t) hart->x_regs[RV_R_A0];
        hart_mask_base = (uint64_t) hart->x_regs[RV_R_A1];
        start_addr = hart->x_regs[RV_R_A2];
        size = hart->x_regs[RV_R_A3];
        // a4 = asid（ASID 版本，當前被忽略）

        if (hart_mask_base == 0xFFFFFFFFFFFFFFFF) {
            // ═══════════════════════════════════════════
            // 失效所有 harts 的 TLB
            // ═══════════════════════════════════════════
            for (uint32_t i = 0; i < hart->vm->n_hart; i++)
                mmu_invalidate_range(hart->vm->hart[i], start_addr, size);
        } else {
            // ═══════════════════════════════════════════
            // 失效指定 harts 的 TLB
            // ═══════════════════════════════════════════
            for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++) {
                if (hart_mask & 1)
                    mmu_invalidate_range(hart->vm->hart[i], start_addr, size);
            }
        }

        return (sbi_ret_t){SBI_SUCCESS, 0};
    }

    case SBI_RFENCE__GVMA_VMID:
    case SBI_RFENCE__GVMA:
    case SBI_RFENCE__VVMA_ASID:
    case SBI_RFENCE__VVMA:
        // ═══════════════════════════════════════════
        // Hypervisor 相關的 RFENCE 操作
        // ═══════════════════════════════════════════
        // SEMU 不支援虛擬化，返回成功但不執行
        return (sbi_ret_t){SBI_SUCCESS, 0};

    default:
        return (sbi_ret_t){SBI_ERR_FAILED, 0};
    }
}
```

**RFENCE 使用場景**：

```
場景 1：Linux 修改頁表
  Hart 0 修改頁表：
    - 修改 PTE（設置/清除權限位）
    - 需要通知其他 harts 失效 TLB

  呼叫 RFENCE.VMA：
    sbi_ecall(SBI_EID_RFENCE, SBI_RFENCE__VMA,
              hart_mask, hart_mask_base,
              start_addr, size, 0, 0);

  結果：
    - 所有指定 harts 的 TLB 中包含該地址範圍的項被失效
    - 下次訪問時重新遍歷頁表

場景 2：Linux 切換進程
  切換進程（改變地址空間）：
    - 寫入新的 satp
    - 本地 TLB 自動失效（mmu_set）
    - 如果其他 harts 運行同一進程，需要 RFENCE

  呼叫 RFENCE.VMA：
    sbi_ecall(SBI_EID_RFENCE, SBI_RFENCE__VMA,
              all_harts_mask, 0,
              0, 0, 0, 0);  // size=0 表示全部失效

場景 3：Linux 取消映射頁面
  munmap() 系統調用：
    - 清除 PTE（V=0）
    - 需要失效 TLB

  呼叫 RFENCE.VMA（範圍失效）：
    sbi_ecall(SBI_EID_RFENCE, SBI_RFENCE__VMA,
              hart_mask, hart_mask_base,
              unmap_addr, unmap_size, 0, 0);
```

**RFENCE 與 TLB 失效優化**：

```c
// 參數 size 的特殊含義
if (size == 0 || size == (uint32_t)-1) {
    // 完全失效（失效所有 TLB 項）
    mmu_invalidate(hart);
} else {
    // 範圍失效（只失效指定範圍）
    mmu_invalidate_range(hart, start_addr, size);
}
```

**為何範圍失效更快？**

```
假設 TLB 有 17 項：
  - 1 項指令快取
  - 8×2 項載入快取
  - 8×2 項儲存快取

完全失效：
  - 失效所有 17 項
  - 下次訪問任何頁面都會未命中

範圍失效（例如 1 個頁面）：
  - 只檢查 17 項，失效匹配的（通常 0-3 項）
  - 其他頁面的快取項保留
  - 顯著減少 TLB miss
```

---

## 🔄 協程式 SMP 實現

### 1. 為何使用協程？

**傳統多執行緒 SMP 的問題**：

```
傳統方式（每個 hart 一個 OS 執行緒）：
  優點：
    - 真正的並行執行
    - 硬體加速（多核心 CPU）

  缺點：
    - 需要複雜的同步（鎖、原子操作）
    - 競態條件（race condition）
    - 除錯困難
    - 記憶體訪問需要保護
```

**協程式 SMP 的優勢**：

```
協程方式（協作式多工）：
  優點：
    - 簡化並發模型（無需鎖）
    - 確定性執行（易於除錯）
    - 輕量級上下文切換
    - 易於實現和理解

  缺點：
    - 不是真正的並行（單執行緒）
    - 無法利用多核心 CPU
    - 對於 SEMU（教育工具），優勢遠大於劣勢
```

### 2. 協程實現原理

#### 平台選擇

**位置**：[coro.c](../coro.c):7-19

```c
/* 自動選擇實現方式 */
#if !defined(CORO_USE_UCONTEXT) && !defined(CORO_USE_ASM)
#if __GNUC__ >= 3
#if defined(__x86_64__) || defined(__aarch64__)
#define CORO_USE_ASM        // x86-64/ARM64 使用彙編（更快）
#else
#define CORO_USE_UCONTEXT   // 其他平台使用 ucontext（通用）
#endif
#else
#define CORO_USE_UCONTEXT
#endif
#endif
```

#### 彙編實現（x86-64）

**位置**：[coro.c](../coro.c):33-96

```c
/* x86-64 上下文緩衝 */
typedef struct {
    void *rip,  // 指令指針
         *rsp,  // 堆疊指針
         *rbp,  // 基址指針
         *rbx,  // 通用暫存器
         *r12, *r13, *r14, *r15;  // 被呼叫者保存的暫存器
} coro_ctxbuf_t;

/* 上下文切換（彙編實現） */
__asm__(
    ".text\n"
    ".globl _coro_switch\n"
    "_coro_switch:\n"
    /* 保存當前上下文（from） */
    "  leaq 0x3d(%rip), %rax\n"  // 計算返回地址
    "  movq %rax, (%rdi)\n"      // 保存 RIP
    "  movq %rsp, 8(%rdi)\n"     // 保存 RSP
    "  movq %rbp, 16(%rdi)\n"    // 保存 RBP
    "  movq %rbx, 24(%rdi)\n"    // 保存 RBX
    "  movq %r12, 32(%rdi)\n"    // 保存 R12
    "  movq %r13, 40(%rdi)\n"    // 保存 R13
    "  movq %r14, 48(%rdi)\n"    // 保存 R14
    "  movq %r15, 56(%rdi)\n"    // 保存 R15

    /* 恢復新上下文（to） */
    "  movq 56(%rsi), %r15\n"    // 恢復 R15
    "  movq 48(%rsi), %r14\n"    // 恢復 R14
    "  movq 40(%rsi), %r13\n"    // 恢復 R13
    "  movq 32(%rsi), %r12\n"    // 恢復 R12
    "  movq 24(%rsi), %rbx\n"    // 恢復 RBX
    "  movq 16(%rsi), %rbp\n"    // 恢復 RBP
    "  movq 8(%rsi), %rsp\n"     // 恢復 RSP
    "  jmpq *(%rsi)\n"           // 跳轉到保存的 RIP
    "  ret\n"
);
```

**為何只保存部分暫存器？**

```
x86-64 呼叫約定：
  - 被呼叫者保存（Callee-saved）：RBX, RBP, R12-R15
    → 函數必須保存這些暫存器
    → 協程切換時需要保存

  - 呼叫者保存（Caller-saved）：RAX, RCX, RDX, RSI, RDI, R8-R11
    → 呼叫函數前由呼叫者保存
    → 協程切換時不需要保存（已經在堆疊上）
```

#### ucontext 實現（通用）

**位置**：[coro.c](../coro.c):200-250

```c
#ifdef CORO_USE_UCONTEXT

#include <ucontext.h>

typedef struct {
    ucontext_t ctx;
} coro_ctxbuf_t;

/* 建立協程上下文 */
void coro_init_context(coro_ctxbuf_t *buf, void *stack, size_t stack_size,
                       void (*entry)(void *), void *arg) {
    getcontext(&buf->ctx);               // 獲取當前上下文
    buf->ctx.uc_stack.ss_sp = stack;     // 設置堆疊
    buf->ctx.uc_stack.ss_size = stack_size;
    buf->ctx.uc_link = NULL;             // 返回時終止
    makecontext(&buf->ctx, entry, 1, arg);  // 設置入口函數
}

/* 切換上下文 */
int coro_switch(coro_ctxbuf_t *from, coro_ctxbuf_t *to) {
    return swapcontext(&from->ctx, &to->ctx);
}

#endif
```

### 3. 協程調度實現

#### 協程狀態

```c
typedef enum {
    CORO_STATE_SUSPENDED,  // 掛起（等待事件）
    CORO_STATE_RUNNING,    // 正在運行
    CORO_STATE_DEAD        // 已結束
} coro_state_t;
```

#### 協程結構

```c
typedef struct coro {
    coro_ctxbuf_t ctx;          // 上下文緩衝
    coro_state_t state;         // 協程狀態
    void *stack;                // 堆疊指針
    size_t stack_size;          // 堆疊大小
    void (*entry)(void *);      // 入口函數
    void *arg;                  // 入口參數
    struct coro *next;          // 鏈表（用於調度）
} coro_t;
```

#### 協程初始化

**位置**：[coro.c](../coro.c):300-400

```c
void coro_init(vm_t *vm, uint32_t n_hart)
{
    // ═══════════════════════════════════════════
    // 為每個 hart 建立協程
    // ═══════════════════════════════════════════
    for (uint32_t i = 0; i < n_hart; i++) {
        hart_t *hart = vm->hart[i];

        // 分配堆疊（8MB）
        size_t stack_size = 8 * 1024 * 1024;
        void *stack = malloc(stack_size);

        // 設置堆疊保護頁（檢測溢出）
        mprotect(stack, 4096, PROT_NONE);

        // 建立協程
        coro_t *co = malloc(sizeof(coro_t));
        co->stack = stack;
        co->stack_size = stack_size;
        co->entry = hart_exec;  // 入口函數
        co->arg = hart;         // 傳遞 hart 指針
        co->state = CORO_STATE_SUSPENDED;

        // 初始化上下文
        coro_init_context(&co->ctx, stack + 4096,  // 跳過保護頁
                          stack_size - 4096,
                          co->entry, co->arg);

        // 儲存協程指針
        hart->priv_coro = co;
    }
}
```

**堆疊保護頁**：

```
堆疊佈局（8MB）：
┌─────────────────────┐ ← stack + 8MB（高地址）
│                     │
│   可用堆疊空間      │   7MB + 4KB - 4KB = ~8MB
│   (PROT_READ|WRITE) │
│                     │
├─────────────────────┤ ← stack + 4KB
│   保護頁（4KB）     │   mprotect(..., PROT_NONE)
│   (不可訪問)        │   訪問會觸發 SIGSEGV
└─────────────────────┘ ← stack（低地址）

作用：
  如果堆疊溢出，訪問保護頁會立即崩潰
  而不是悄悄損壞其他數據
```

#### 協程調度器

**位置**：[coro.c](../coro.c):400-500

```c
void coro_schedule(vm_t *vm)
{
    // ═══════════════════════════════════════════
    // 選擇下一個可運行的 hart
    // ═══════════════════════════════════════════

    static uint32_t last_hart = 0;  // LRU 調度

    // 從 last_hart 開始輪詢
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        uint32_t idx = (last_hart + i) % vm->n_hart;
        hart_t *hart = vm->hart[idx];
        coro_t *co = hart->priv_coro;

        // 跳過非 STARTED 的 hart
        if (hart->hsm_status != SBI_HSM_STATE_STARTED)
            continue;

        // 跳過掛起的協程
        if (co->state == CORO_STATE_SUSPENDED)
            continue;

        // 找到可運行的 hart
        last_hart = idx;
        current_coro = co;

        // 切換到該協程
        coro_switch(&scheduler_ctx, &co->ctx);

        return;
    }

    // ═══════════════════════════════════════════
    // 沒有可運行的 hart → 事件驅動等待
    // ═══════════════════════════════════════════
    coro_wait_for_event(vm);
}
```

#### 協程讓出（WFI 處理）

```c
void semu_wfi(hart_t *hart)
{
    coro_t *co = hart->priv_coro;

    // 標記為掛起
    co->state = CORO_STATE_SUSPENDED;
    hart->in_wfi = true;

    // 切換回調度器
    coro_switch(&co->ctx, &scheduler_ctx);

    // 被喚醒後繼續執行
    hart->in_wfi = false;
    co->state = CORO_STATE_RUNNING;
}
```

### 4. 事件驅動等待

**位置**：[coro.c](../coro.c):500-615

```c
void coro_wait_for_event(vm_t *vm)
{
    // ═══════════════════════════════════════════
    // 計算下一個事件的時間
    // ═══════════════════════════════════════════
    uint64_t next_event = UINT64_MAX;

    // 檢查所有 harts 的計時器
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        emu_state_t *data = vm->hart[i]->priv;
        uint64_t mtimecmp = data->mtimer.mtimecmp[i];

        if (mtimecmp < next_event)
            next_event = mtimecmp;
    }

    // 計算需要等待的時間
    uint64_t current_time = get_current_time();
    if (next_event <= current_time) {
        // 已經超時，立即返回
        return;
    }

    uint64_t wait_ns = (next_event - current_time) * 100;  // 10MHz → ns

#ifdef __APPLE__
    // ═══════════════════════════════════════════
    // macOS：使用 kqueue
    // ═══════════════════════════════════════════
    static int kq = -1;
    if (kq < 0)
        kq = kqueue();

    struct kevent kev;
    struct timespec timeout = {
        .tv_sec = wait_ns / 1000000000,
        .tv_nsec = wait_ns % 1000000000
    };

    // 等待事件或超時
    kevent(kq, NULL, 0, &kev, 1, &timeout);

#else
    // ═══════════════════════════════════════════
    // Linux：使用 timerfd
    // ═══════════════════════════════════════════
    static int tfd = -1;
    if (tfd < 0) {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    }

    struct itimerspec new_value = {
        .it_value = {
            .tv_sec = wait_ns / 1000000000,
            .tv_nsec = wait_ns % 1000000000
        }
    };

    timerfd_settime(tfd, 0, &new_value, NULL);

    // 等待計時器觸發
    uint64_t exp;
    read(tfd, &exp, sizeof(exp));
#endif
}
```

**事件驅動的優勢**：

```
輪詢方式（舊版）：
  while (all_harts_in_wfi) {
      usleep(100);  // 睡眠 100μs
      check_events();
  }

  問題：
    - 浪費 CPU（即使沒事做也要醒來）
    - 響應延遲（最多 100μs）
    - 功耗高

事件驅動方式（新版）：
  wait_time = min(mtimecmp) - current_time;
  kevent/timerfd_wait(wait_time);  // 精確等待

  優勢：
    - 不浪費 CPU（OS 會睡眠）
    - 精確喚醒（到時間立即醒來）
    - 功耗低
```

---

## 🎓 實踐練習

### 練習 1：追蹤 SBI 呼叫

**目標**：觀察 Linux 如何呼叫 SBI

```bash
# 啟動 SEMU（GDB 模式）
./build/semu -k build/Image -g 1234 &

# 啟動 GDB
riscv32-unknown-elf-gdb
(gdb) target remote :1234
(gdb) break handle_sbi_ecall
(gdb) continue

# 到達斷點
(gdb) print/x hart->x_regs[17]  # a7 = Extension ID
(gdb) print/x hart->x_regs[16]  # a6 = Function ID
(gdb) print/x hart->x_regs[10]  # a0 = 參數 0
(gdb) continue
```

### 練習 2：啟動次級核心

**目標**：觀察多核心啟動過程

```bash
# 編譯 4 核心版本
make clean && make SMP=4

# 運行並觀察
./build/semu -k build/Image

# 在 Linux 中檢查
cat /proc/cpuinfo  # 應該看到 4 個 CPU
```

### 練習 3：發送 IPI

**目標**：觀察 IPI 如何喚醒其他核心

```bash
(gdb) break handle_sbi_ecall_IPI
(gdb) continue

# 到達斷點
(gdb) print/x hart->x_regs[10]  # a0 = hart_mask
(gdb) print/x hart->x_regs[11]  # a1 = hart_mask_base
(gdb) step
(gdb) print data->sswi.ssip[0]
(gdb) print data->sswi.ssip[1]
```

### 練習 4：觀察 RFENCE

**目標**：追蹤 TLB 失效過程

```bash
(gdb) break handle_sbi_ecall_RFENCE
(gdb) continue

# 到達斷點
(gdb) print/x start_addr
(gdb) print/x size
(gdb) step
# 觀察 mmu_invalidate_range 被呼叫
```

### 練習 5：協程切換

**目標**：觀察協程如何切換

```bash
(gdb) break coro_schedule
(gdb) continue

# 到達斷點
(gdb) print last_hart
(gdb) print current_coro->state
(gdb) step
# 觀察 coro_switch 被呼叫
```

---

## 📊 知識檢查點

### 基礎問題

1. **Q**: SBI 是什麼？
   **A**: Supervisor Binary Interface，S-mode 與 M-mode 之間的標準介面

2. **Q**: SBI ecall 使用哪些暫存器傳遞參數？
   **A**: a7=EID, a6=FID, a0-a5=參數，返回 a0=error, a1=value

3. **Q**: Linux 如何設置計時器中斷？
   **A**: 通過 SBI TIMER 擴展的 SET_TIMER 函數

4. **Q**: HSM 擴展有哪些功能？
   **A**: HART_START, HART_STOP, HART_SUSPEND, HART_GET_STATUS

5. **Q**: IPI 如何工作？
   **A**: 通過設置目標 hart 的 ssip 暫存器，觸發軟體中斷

### 進階問題

1. **Q**: HART_SUSPEND 的兩種模式有何區別？
   **A**: Retentive 保持狀態返回原地，Non-retentive 不保持狀態跳轉到新地址

2. **Q**: RFENCE 與本地 FENCE 有何區別？
   **A**: RFENCE 失效其他 harts 的 TLB，FENCE 只影響當前 hart

3. **Q**: 為何使用協程而非多執行緒？
   **A**: 簡化並發模型，確定性執行，易於除錯，適合教育工具

4. **Q**: 協程的堆疊保護頁有何作用？
   **A**: 檢測堆疊溢出，訪問保護頁會立即觸發 SIGSEGV

5. **Q**: 事件驅動等待比輪詢有何優勢？
   **A**: 不浪費 CPU，精確喚醒，功耗低

### 深入問題

1. **Q**: hart_mask 如何編碼多個目標 hart？
   **A**: 位元遮罩，bit i=1 表示 hart (base+i) 是目標

2. **Q**: RFENCE.VMA 的 size=0 有何特殊含義？
   **A**: 表示完全失效（失效所有 TLB 項）

3. **Q**: 協程切換為何只保存部分暫存器？
   **A**: 遵循呼叫約定，只保存 callee-saved 暫存器

4. **Q**: WFI 如何觸發協程讓出？
   **A**: hart->wfi() 回調設置為 semu_wfi()，內部呼叫 coro_switch

5. **Q**: 如何防止 HART_STOP 後的 hart 被執行？
   **A**: 在 vm_step() 開頭檢查 hsm_status，非 STARTED 直接返回

---

## 🎯 下一階段預告

完成本階段後，你已經掌握了 SBI 實現與協程式調度。

**階段 4** 將深入研究：
- PLIC 平台中斷控制器
- ACLINT（MTIMER、MSWI、SSWI）
- 中斷路由與處理流程
- 中斷優先級與遮罩

繼續閱讀：[階段 4：中斷系統](./stage4-interrupts.md)

---

**文檔版本**：v1.0
**最後更新**：2025-01-10
**預估學習時間**：2-3 天
