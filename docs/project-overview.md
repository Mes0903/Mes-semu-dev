# SEMU 專案架構概覽

> RISC-V 系統模擬器完整架構說明

---

## 📖 文檔導覽

- **本文檔**：專案整體架構與設計理念
- [數據結構詳解](./data-structures.md)：核心數據結構定義
- [閱讀計畫索引](./reading-plan-index.md)：完整學習路徑

---

## 🎯 專案簡介

**semu** 是一個極簡主義的 RISC-V 系統模擬器，能夠運行完整的 Linux 內核及用戶空間程序。

### 核心特性

- **ISA 支援**：RV32IMA（32位整數、乘法、原子操作）
- **特權級別**：S-mode（Supervisor）和 U-mode（User）
- **虛擬記憶體**：完整的 Sv32 MMU，配備 8×2 組相聯 TLB 快取
- **多核心**：協程式 SMP 支援（最多 4095 個 harts）
- **中斷系統**：PLIC + ACLINT（MTIMER、MSWI、SSWI）
- **SBI 實現**：完整的 SBI 0.2 規範（包含 HSM、IPI、RFENCE 擴展）
- **外設模擬**：UART、VirtIO（Block、Net、FS、Sound、RNG）

### 代碼規模

```
核心代碼：   ~8,594 行 C
頭文件：     ~1,436 行
總計：       ~10,000 行
```

**設計理念**：精簡但不簡陋，高可讀性，教育導向

---

## 🏗️ 系統架構圖

### 整體架構

```
┌─────────────────────────────────────────────────────────────┐
│                      用戶空間應用程式                          │
│                  (busybox, shell, utilities)                 │
└────────────────────┬────────────────────────────────────────┘
                     │ syscall
┌────────────────────▼────────────────────────────────────────┐
│                      Linux Kernel                            │
│                    (S-mode + U-mode)                         │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Scheduler │ MM │ VFS │ Network │ Drivers │ ...     │   │
│  └──────────────────────────────────────────────────────┘   │
└────────────────────┬────────────────────────────────────────┘
                     │ SBI ecall (S-mode → M-mode)
┌────────────────────▼────────────────────────────────────────┐
│                    SEMU Emulator (M-mode)                    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              SBI 0.2 Implementation                  │    │
│  │  (Timer, IPI, HSM, RFENCE, System Reset)            │    │
│  └─────────────────────────────────────────────────────┘    │
│                                                               │
│  ┌─────────────────────────────────────────────────────┐    │
│  │          RISC-V CPU Core (hart_t)                   │    │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐            │    │
│  │  │ Decoder  │ │   ALU    │ │   MMU    │            │    │
│  │  └──────────┘ └──────────┘ └──────────┘            │    │
│  │  ┌──────────────────────────────────────┐           │    │
│  │  │  8×2 Set-Associative TLB Cache       │           │    │
│  │  └──────────────────────────────────────┘           │    │
│  └─────────────────────────────────────────────────────┘    │
│                          │                                   │
│  ┌───────────────────────▼───────────────────────────────┐  │
│  │              Memory Subsystem                         │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐            │  │
│  │  │ RAM      │  │  MMIO    │  │   MMU    │            │  │
│  │  │ (512MB)  │  │ Devices  │  │PageTable │            │  │
│  │  └──────────┘  └──────────┘  └──────────┘            │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌───────────────────────────────────────────────────────┐  │
│  │            Interrupt Controllers                      │  │
│  │  ┌────────┐  ┌────────────────────────────┐          │  │
│  │  │  PLIC  │  │       ACLINT              │          │  │
│  │  │ (32 IRQ)  │  MTIMER │ MSWI │ SSWI │   │          │  │
│  │  └────────┘  └────────────────────────────┘          │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                 I/O Devices                           │  │
│  │  ┌─────────┐  ┌──────────────────────────────────┐   │  │
│  │  │  UART   │  │      VirtIO Devices             │   │  │
│  │  │  8250   │  │  Block│Net│FS│Sound│RNG│       │   │  │
│  │  └─────────┘  └──────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌───────────────────────────────────────────────────────┐  │
│  │         Coroutine-based SMP Scheduler                 │  │
│  │  (Event-driven, WFI-yielding, up to 4095 harts)      │  │
│  └───────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│                    Host System                               │
│  (Linux/macOS: stdio, files, network, audio)                │
└──────────────────────────────────────────────────────────────┘
```

---

## 📁 目錄結構與模組劃分

### 核心源碼（主模擬器）

```
Mes-semu/
├── main.c           (1537 行) ⭐⭐⭐
│   ├── main() / semu_init()      - 系統初始化
│   ├── Memory Mapping            - 記憶體映射（RAM + MMIO）
│   ├── SBI Implementation        - SBI 0.2 完整實現
│   │   ├── BASE / TIMER          - 基礎與計時器
│   │   ├── HSM                   - Hart 狀態管理（多核心）
│   │   ├── IPI                   - 處理器間中斷
│   │   └── RFENCE                - 遠程柵欄（TLB 同步）
│   └── Device Initialization     - 設備初始化
│
├── riscv.c          (1074 行) ⭐⭐⭐
│   ├── vm_step()                 - 主執行循環
│   ├── Instruction Decoder       - RV32IMA 指令解碼
│   ├── MMU Implementation        - Sv32 分頁系統
│   │   ├── mmu_translate()       - 頁表遍歷
│   │   ├── 8×2 Set-Associative   - 組相聯快取
│   │   └── mmu_invalidate_range()- 智能快取失效
│   ├── Exception Handling        - 異常處理
│   └── CSR Operations            - 控制狀態暫存器
│
├── riscv.h          (203 行)
│   ├── hart_t                    - CPU 核心結構
│   ├── vm_t                      - 多核心容器
│   └── MMU Cache Structures      - 快取結構定義
│
└── riscv_private.h  (158 行)
    ├── Instruction Encoding      - 指令編碼常數
    ├── CSR Definitions           - CSR 暫存器地址
    ├── Exception Codes           - 異常代碼
    └── SBI Constants             - SBI 擴展定義
```

### 記憶體與中斷控制器

```
├── ram.c            (69 行)
│   └── RAM Read/Write Operations - 記憶體讀寫
│
├── plic.c           (128 行) ⭐⭐
│   ├── PLIC State Management     - PLIC 狀態管理
│   ├── Interrupt Routing         - 中斷路由（32 源）
│   └── Priority Handling         - 優先級處理（簡化版）
│
└── aclint.c         (227 行) ⭐⭐
    ├── MTIMER                    - 64位元計時器
    │   ├── mtime                 - 當前時間
    │   └── mtimecmp[]            - 每個 hart 的比較值
    ├── MSWI                      - M-mode 軟體中斷
    │   └── msip[]                - IPI 暫存器
    └── SSWI                      - S-mode 軟體中斷
        └── ssip[]                - S-mode IPI
```

### 外設模擬

```
├── uart.c           (259 行) ⭐
│   ├── 8250/16550 UART           - 串口模擬
│   ├── Register Emulation        - 暫存器模擬
│   └── Coroutine Input Wait      - 協程式輸入等待
│
├── virtio-blk.c     (481 行) ⭐⭐
│   ├── VirtQueue Management      - 隊列管理
│   ├── Block Device Operations   - 塊設備操作
│   └── ext4 Disk Image Support   - 磁碟映像支援
│
├── virtio-net.c     (654 行) ⭐⭐
│   ├── Network Device Emulation  - 網路設備模擬
│   ├── TX/RX Queue Handling      - 發送/接收隊列
│   └── Multiple Backends         - 多種後端（TAP/vmnet/SLIRP）
│
├── virtio-fs.c      (959 行) ⭐
│   ├── FUSE Protocol             - FUSE 協議實現
│   ├── Inode Mapping             - inode 映射表
│   └── Host Directory Sharing    - 主機目錄共享
│
├── virtio-snd.c     (1258 行) ⭐
│   ├── Audio Playback            - 音效播放
│   ├── PortAudio Integration     - PortAudio 整合
│   └── PCM Stream Management     - PCM 串流管理
│
├── virtio-rng.c     (264 行)
│   └── Random Number Generator   - 隨機數產生器
│
└── virtio.h         (113 行)
    └── VirtIO Common Definitions - VirtIO 共用定義
```

### 網路支持

```
├── netdev.c         (158 行)
│   └── Network Device Abstraction- 網路設備抽象層
│
├── netdev-vmnet.c   (490 行)
│   └── macOS vmnet Backend       - macOS 網路後端
│
├── slirp.c          (243 行)
│   └── User-mode Networking      - 用戶模式網路
│
└── minislirp/       (submodule)
    └── Lightweight SLIRP         - 輕量級 SLIRP 實現
```

### 多核心與調度

```
├── coro.c           (615 行) ⭐⭐⭐
│   ├── Coroutine Framework       - 協程框架
│   ├── SMP Hart Scheduling       - SMP 調度器
│   ├── WFI Handling              - WFI 協程讓出
│   └── Event-driven Wakeup       - 事件驅動喚醒
│       ├── kqueue (macOS)        - macOS 事件機制
│       └── timerfd (Linux)       - Linux 計時器
│
└── coro.h           (40 行)
    └── Coroutine API             - 協程 API 定義
```

### 工具與配置

```
├── utils.c          (178 行)
│   ├── Timer Functions           - 計時器函數
│   └── List Operations           - 鏈表操作
│
├── utils.h          (118 行)
│   └── Utility Functions         - 工具函數定義
│
├── device.h         (462 行) ⭐⭐⭐
│   ├── emu_state_t               - 頂層狀態結構
│   ├── All Device States         - 所有設備狀態
│   └── Function Declarations     - 函數聲明
│
├── common.h         (55 行)
│   ├── Utility Macros            - 工具宏
│   └── Compiler Hints            - 編譯器提示
│
└── feature.h        (27 行)
    └── Compile-time Features     - 編譯時功能開關
```

### 構建與配置

```
├── Makefile                      - 主構建文件
├── minimal.dts                   - 設備樹源碼
├── configs/                      - 配置文件
│   ├── buildroot.config          - Buildroot 配置
│   └── linux.config              - Linux 內核配置
│
└── scripts/                      - 構建腳本
    ├── build-image.sh            - 自動構建腳本
    ├── gen-hart-dts.py           - 動態設備樹生成
    └── verify-dtb.sh             - 設備樹驗證
```

### 外部依賴（子模組）

```
├── mini-gdbstub/                 - GDB 調試支持
├── minislirp/                    - 用戶模式網路堆疊
└── portaudio/                    - 跨平台音效庫
```

---

## 🔑 六大核心區域

### 1️⃣ CPU 核心與指令執行

**文件**：[riscv.c](../riscv.c)、[riscv.h](../riscv.h)、[riscv_private.h](../riscv_private.h)

**功能**：
- RV32IMA 指令集解碼執行
- Sv32 虛擬記憶體管理
- 8×2 組相聯 TLB 快取（最新優化）
- 異常處理與特權級切換

**核心結構**：
```c
hart_t {
    x_regs[32]              // 通用暫存器
    pc, current_pc          // 程序計數器
    MMU caches:
      cache_fetch           // 指令快取（直接映射）
      cache_load[8]         // 載入快取（8組×2路）
      cache_store[8]        // 儲存快取（8組×2路）
    CSRs (S-mode)           // 控制狀態暫存器
    Callbacks               // 環境回調函數
}
```

**關鍵技術**：
- **3-bit parity hash** 索引（減少快取衝突）
- **LRU 替換策略**
- **零拷貝記憶體訪問**（指令快取存主機指針）

---

### 2️⃣ 系統管理與 SBI

**文件**：[main.c](../main.c)

**功能**：
- SBI 0.2 標準實現
- 記憶體映射管理（512MB RAM + MMIO）
- 設備樹載入
- 系統初始化與啟動

**SBI 擴展**：
```c
SBI_EID_BASE     // 基礎擴展（版本查詢、功能探測）
SBI_EID_TIMER    // 計時器（設置 MTIMECMP）
SBI_EID_RST      // 系統重置
SBI_EID_HSM      // Hart 狀態管理（多核心啟動/停止/掛起）
SBI_EID_IPI      // 處理器間中斷
SBI_EID_RFENCE   // 遠程柵欄（TLB 刷新）
```

**記憶體映射**：
```
0x00000000 - 0x07FFFFFF   RAM (128MB 默認, 可配置到 512MB)
0xF0000000 - 0xF3FFFFFF   PLIC (64MB)
0xF4000000 - 0xF40FFFFF   UART (1MB)
0xF4100000 - 0xF41FFFFF   VirtIO-Net (1MB)
0xF4200000 - 0xF42FFFFF   VirtIO-Blk (1MB)
0xF4300000 - 0xF43FFFFF   ACLINT MTIMER (1MB)
0xF4400000 - 0xF44FFFFF   ACLINT MSWI (1MB)
0xF4500000 - 0xF45FFFFF   ACLINT SSWI (1MB)
0xF4600000 - 0xF48FFFFF   其他 VirtIO 設備
```

---

### 3️⃣ 多核心調度系統

**文件**：[coro.c](../coro.c)、[coro.h](../coro.h)

**創新設計**：協程式 SMP（而非多執行緒）

**核心機制**：
```
WFI 指令 → coro_yield() → 切換到其他 hart
  ↓
事件發生（計時器/UART/中斷）
  ↓
coro_schedule() → 喚醒對應 hart
```

**優勢**：
- 簡化並發模型（無需鎖）
- 事件驅動（不浪費 CPU）
- 易於調試

**實現技術**：
- **ucontext**：輕量級協程
- **kqueue/timerfd**：高效等待
- **堆疊溢出檢測**

---

### 4️⃣ 中斷系統

**文件**：[plic.c](../plic.c)、[aclint.c](../aclint.c)

**三層架構**：

```
外設設備（UART、VirtIO）
    ↓
PLIC（平台級中斷控制器）
    ├── 32 中斷源
    ├── 中斷優先級（簡化版）
    └── 多上下文支援
    ↓
hart.sip（S-mode 中斷待處理）
    ├── SSI（軟體中斷）
    ├── STI（計時器中斷）
    └── SEI（外部中斷）
    ↓
hart_trap()（陷阱處理）
```

**ACLINT 組件**：
- **MTIMER**：64位元計時器（Linux 調度器）
- **MSWI**：M-mode IPI（SBI 使用）
- **SSWI**：S-mode IPI（OS 使用）

---

### 5️⃣ 外設與 VirtIO 設備

**文件**：[uart.c](../uart.c)、`virtio-*.c`

**設備列表**：
- **UART (8250)**：串口終端
- **VirtIO-Block**：磁碟（ext4 映像）
- **VirtIO-Net**：網路（TAP/vmnet/SLIRP）
- **VirtIO-FS**：文件系統共享（FUSE）
- **VirtIO-Sound**：音效（PortAudio）
- **VirtIO-RNG**：隨機數產生器

**VirtIO 統一架構**：
```c
virtio_xxx_state_t {
    Feature Negotiation     // 功能協商
    Queue Configuration     // 隊列配置
    Status & Interrupt      // 狀態與中斷
    Device-specific Fields  // 設備專屬欄位
}
```

**VirtQueue 機制**（環緩衝）：
```
Descriptor Table → Available Ring → Device Processing
                                            ↓
Guest Interrupt ← Used Ring Update ← Completion
```

---

### 6️⃣ 工具與基礎設施

**文件**：[ram.c](../ram.c)、[utils.c](../utils.c)、[device.h](../device.h)

**提供功能**：
- 記憶體讀寫操作
- 計時器與時間管理
- 鏈表操作
- 工具宏（MASK、RANGE_CHECK、unlikely/likely）
- 設備狀態結構定義

---

## 🔄 數據流與執行流程

### 系統啟動流程

```
1. main()
   ├── 解析命令列參數
   ├── semu_init()
   │   ├── 分配 512MB RAM (mmap)
   │   ├── 載入 kernel Image → RAM[0x0]
   │   ├── 載入 initrd → RAM[403MB]
   │   ├── 載入 DTB → RAM[511MB]
   │   ├── 初始化設備（UART、VirtIO）
   │   ├── 設置記憶體映射回調
   │   └── 初始化 harts
   │       ├── hart[0]: PC=0x0, STARTED
   │       └── hart[1..N]: STOPPED
   │
   ├── coro_init()
   │   └── 為每個 hart 建立協程
   │
   └── hart_exec_loop()
       └── while (!stopped)
           ├── coro_schedule() → 切換到可運行 hart
           ├── vm_step() → 執行指令
           ├── 檢查中斷（ACLINT、PLIC）
           └── 更新外設狀態

2. Linux Kernel Boot
   ├── SBI ecall → handle_sbi_ecall()
   ├── Timer setup → MTIMER
   ├── 次級核心啟動 → SBI_HSM__HART_START
   └── 進入用戶空間
```

### 指令執行流程

```
vm_step(hart)
    ↓
1. 檢查 error 狀態
    ↓
2. 指令抓取（Fetch）
   ├── 查詢 cache_fetch
   ├── 命中 → 直接讀取
   └── 未命中 → mem_fetch() → 更新快取
    ↓
3. 指令解碼（Decode）
   ├── 提取 opcode
   ├── 提取 funct3/funct7
   └── 提取立即數/暫存器
    ↓
4. 執行（Execute）
   ├── ALU 運算
   ├── 記憶體訪問（Load/Store）
   │   ├── MMU 轉譯
   │   │   ├── 查詢 cache_load/store
   │   │   ├── 未命中 → 頁表遍歷
   │   │   └── 更新快取（LRU）
   │   └── mem_load/store() 回調
   ├── 分支/跳轉
   └── CSR 操作
    ↓
5. 更新 PC
    ↓
6. 檢查中斷
   if (sip & sie & sstatus_sie)
       → hart_trap()
```

### 中斷處理流程

```
外設觸發中斷
    ↓
PLIC 更新
    plic.active |= (1 << IRQ_NUM)
    plic.ip |= (1 << IRQ_NUM)
    ↓
if (plic.ie[context] & bit)
    hart->sip |= RV_INT_SEI_BIT
    ↓
vm_step() 檢查中斷條件
    if (sip & sie & sstatus_sie)
        ↓
        hart_trap(hart)
            ├── 保存狀態
            │   ├── sepc = pc
            │   ├── scause = 中斷原因
            │   ├── stval = 0
            │   ├── sstatus_spie = sstatus_sie
            │   └── sstatus_spp = s_mode
            ├── 進入 S-mode
            │   └── s_mode = true
            ├── 禁用中斷
            │   └── sstatus_sie = false
            └── 跳轉到 stvec_addr
                ↓
                Linux Interrupt Handler
```

### SBI ecall 處理流程

```
S-mode 執行 ecall 指令
    ↓
vm_step() 觸發 RV_EXC_ECALL_S 異常
    ↓
hart_trap() 不處理，返回 main.c
    ↓
handle_sbi_ecall(hart, state)
    ├── 讀取 a7（擴展 ID）
    ├── 讀取 a6（函數 ID）
    ├── 讀取 a0-a5（參數）
    ↓
    switch (a7) {
        case SBI_EID_TIMER:
            → 設置 mtimecmp[hartid]
        case SBI_EID_HSM:
            → 啟動/停止/掛起 hart
        case SBI_EID_IPI:
            → 設置 target_hart->sip |= SSI_BIT
        case SBI_EID_RFENCE:
            → 失效目標 harts 的 TLB
        ...
    }
    ↓
    設置返回值（a0 = result, a1 = value）
    sepc += 4（跳過 ecall 指令）
    清除 error 狀態
```

### MMU 轉譯流程

```
虛擬地址 (VA)
    ↓
1. 計算 VPN 和 Offset
   vpn = VA >> 12
   offset = VA & 0xFFF
    ↓
2. 查詢快取
   set_idx = __builtin_parity(vpn) & 0x7
   set = &cache_load[set_idx]
    ↓
   for (i = 0; i < 2; i++) {  // 2-way
       if (set->ways[i].n_pages == vpn) {
           → 快取命中
           ppn = set->ways[i].phys_ppn
           更新 LRU
           goto done;
       }
   }
    ↓
3. 快取未命中 → 頁表遍歷
   ├── 讀取 satp 獲取根頁表 PPN
   ├── Level 1:
   │   ├── pte1_addr = (satp.ppn << 12) + (vpn[31:22] * 4)
   │   ├── pte1 = mem_page_table(pte1_ppn)[vpn[31:22]]
   │   └── if (pte1.V && pte1.R/W/X) → 大頁（4MB）
   ├── Level 0:
   │   ├── pte0_addr = (pte1.ppn << 12) + (vpn[21:12] * 4)
   │   ├── pte0 = mem_page_table(pte0_ppn)[vpn[21:12]]
   │   └── if (!pte0.V || !權限) → Page Fault
   └── ppn = pte0.ppn
    ↓
4. 更新快取
   way_to_replace = set->lru
   set->ways[way_to_replace].n_pages = vpn
   set->ways[way_to_replace].phys_ppn = ppn
   set->lru = 1 - way_to_replace  // 切換 LRU
    ↓
5. 返回物理地址
   PA = (ppn << 12) | offset
```

---

## 💡 核心設計理念

### 1. 極簡主義

**目標**：用最少的代碼實現完整功能

**實踐**：
- 簡化的 PLIC（無優先級）
- FENCE.I 被忽略（解釋器模式）
- 單執行緒模擬（協程式）

### 2. 高可讀性

**目標**：代碼即文檔

**實踐**：
- 詳細的註釋（尤其是複雜演算法）
- 清晰的命名（`mmu_translate`、`hart_trap`）
- 結構化的函數設計

### 3. 模組化

**目標**：各模組獨立、可替換

**實踐**：
- 統一的回調介面（`mem_load/store`）
- VirtIO 設備共享框架
- 條件編譯（`SEMU_HAS`）

### 4. 性能與正確性平衡

**目標**：在教育工具和實用工具間取得平衡

**實踐**：
- MMU 快取加速（但保留正確性）
- 事件驅動（不浪費 CPU）
- 保留調試能力（GDB stub）

---

## 🚀 最近的重要更新

### MMU 快取升級（2024 Q4）

**變更**：直接映射 → 8×2 組相聯

**原因**：減少快取衝突，提升 MMU 性能

**關鍵 commits**：
```
e9fa09a - Upgrade MMU cache to 8×2 set-associative
ebe49d9 - Fix mmu_invalidate_range for 8×2 set-associative
fb11de8 - Reduce RFENCE IPI cache flush scope
```

**技術細節**：
- 使用 3-bit parity hash 索引
- LRU 替換策略
- 精細化的範圍失效

### 協程式 SMP 支援（2024 Q3）

**變更**：輪詢 → 事件驅動協程

**原因**：降低 CPU 佔用，提升響應速度

**關鍵 commits**：
```
e4ae87e - Add coroutine-based SMP support with WFI
137d4d0 - Refine coroutine for hart scheduling
1aab2cb - Implement event-driven UART coroutine
d06f1cb - Replace usleep with event-driven wait for WFI
5093626 - Add stack overflow detection for coroutines
```

**技術細節**：
- WFI 指令觸發協程讓出
- UART 輸入喚醒等待的 hart
- 使用 kqueue (macOS) / timerfd (Linux)

---

## 📊 性能特性

### 優化手段

1. **MMU 快取**
   - 避免每次記憶體訪問都遍歷頁表
   - 8×2 組相聯，命中率 > 95%

2. **指令抓取快取**
   - 儲存主機記憶體指針
   - 零拷貝訪問

3. **零拷貝記憶體**
   - RAM 訪問直接操作主機記憶體
   - 無需額外複製

4. **事件驅動**
   - WFI 時使用系統等待機制
   - 不消耗 CPU

### 已知限制

1. **單執行緒模擬**
   - 使用協程而非真正的並行執行
   - 多核心是時間片輪轉

2. **無指令快取模擬**
   - FENCE.I 被忽略
   - 適用於解釋器模式

3. **簡化的 PLIC**
   - 無中斷優先級
   - 適用於大多數場景

4. **RV32 限制**
   - 僅支援 32 位元
   - 無法運行 RV64 代碼

---

## 🎓 學習路徑建議

### 快速瀏覽（2-3 天）

1. 閱讀 [專案架構概覽](./project-overview.md)（本文檔）
2. 瀏覽 [數據結構詳解](./data-structures.md)
3. 運行模擬器，觀察啟動過程

### 深入學習（1-2 週）

1. **階段 1**：[建立整體概念](./stage1-overview.md)
2. **階段 2**：[CPU 核心機制](./stage2-cpu-core.md)
3. **階段 3**：[系統服務層](./stage3-system-services.md)
4. **階段 4**：[中斷系統](./stage4-interrupts.md)
5. **階段 5**：[外設設備](./stage5-peripherals.md)

### 完全掌握（3-4 週）

在深入學習的基礎上：
- 完成所有實戰練習
- 閱讀所有源碼文件
- 嘗試修改和擴展功能

---

## 📚 相關資源

### RISC-V 規範

- [RISC-V ISA Manual](https://riscv.org/technical/specifications/)
- [RISC-V Privileged Specification](https://riscv.org/technical/specifications/)
- [SBI Specification 0.2](https://github.com/riscv-non-isa/riscv-sbi-doc)

### VirtIO 規範

- [VirtIO Specification 1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)

### ACLINT 規範

- [RISC-V ACLINT Specification](https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc)

---

## 🛠️ 開發工具

### 編譯與運行

```bash
# 編譯模擬器
make

# 構建 Linux 映像
./scripts/build-image.sh

# 運行模擬器
build/semu -k build/Image

# 多核心模式
make SMP=4
build/semu -k build/Image
```

### 除錯

```bash
# GDB 調試
build/semu -k build/Image -g 1234
gdb build/semu
(gdb) target remote :1234

# 查看 MMU 快取統計
kill -SIGUSR1 <semu_pid>
```

### 測試

```bash
# 運行測試
make check

# 驗證設備樹
./scripts/verify-dtb.sh build/minimal.dtb
```

---

## 📖 文檔索引

- [專案架構概覽](./project-overview.md)（本文檔）
- [數據結構詳解](./data-structures.md)
- [閱讀計畫索引](./reading-plan-index.md)
- [階段 1：建立整體概念](./stage1-overview.md)
- [階段 2：CPU 核心機制](./stage2-cpu-core.md)
- [階段 3：系統服務層](./stage3-system-services.md)
- [階段 4：中斷系統](./stage4-interrupts.md)
- [階段 5：外設設備](./stage5-peripherals.md)
- [階段 6：進階主題](./stage6-advanced.md)

---

**文檔版本**：v1.0
**最後更新**：2025-01-10
**維護者**：Mes
