# 階段 1：建立整體概念

> 理解 SEMU 專案的整體架構與啟動流程（預估 1-2 天）

---

## 📋 本階段目標

- ✅ 理解 SEMU 的設計理念與整體架構
- ✅ 掌握系統啟動流程
- ✅ 熟悉關鍵數據結構的作用
- ✅ 了解記憶體映射佈局
- ✅ 理解設備樹的作用

---

## 📚 閱讀清單

### 必讀文檔

1. [README.md](../README.md) - 專案使用說明
2. [專案架構概覽](./project-overview.md) - 整體架構
3. [數據結構詳解](./data-structures.md) - 核心結構定義

### 必讀代碼

1. [main.c](../main.c):1-200 - `main()` 和 `semu_init()`
2. [device.h](../device.h):428-461 - `emu_state_t` 結構
3. [riscv.h](../riscv.h):78-170 - `hart_t` 結構
4. [minimal.dts](../minimal.dts) - 設備樹源碼

---

## 🎯 核心概念

### 1. SEMU 是什麼？

SEMU (Simple Emulator for RISC-V) 是一個**教育導向**的 RISC-V 系統模擬器。

**核心特點**：
- **極簡**：~10K 行代碼實現完整功能
- **實用**：可運行完整的 Linux 系統
- **高可讀**：代碼即文檔，適合學習

**與其他模擬器的區別**：

| 特性 | QEMU | Spike | SEMU |
|------|------|-------|------|
| 目標 | 生產使用 | ISA 驗證 | 教育學習 |
| 代碼量 | ~500K 行 | ~50K 行 | ~10K 行 |
| 性能 | 極高（JIT） | 中等 | 中等 |
| 可讀性 | 低（複雜） | 中等 | 高（簡潔） |
| 設備支援 | 極豐富 | 極簡 | 適中 |

---

### 2. 系統層次結構

```
┌─────────────────────────────────────────┐
│       用戶空間應用程式                   │  ← U-mode
│    (busybox, shell, utilities)          │
└──────────────┬──────────────────────────┘
               │ syscall
┌──────────────▼──────────────────────────┐
│         Linux Kernel                     │  ← S-mode
│  (Memory Management, VFS, Scheduler)    │
└──────────────┬──────────────────────────┘
               │ SBI ecall (S → M)
┌──────────────▼──────────────────────────┐
│      SEMU (Machine Mode Firmware)       │  ← M-mode
│  - SBI 實現                              │  (模擬器充當)
│  - 設備模擬                              │
│  - 中斷控制                              │
└─────────────────────────────────────────┘
```

**特權級別**：
- **M-mode (Machine)**：最高特權，SEMU 模擬器充當這一層
- **S-mode (Supervisor)**：Linux 內核運行在這一層
- **U-mode (User)**：用戶程式運行在這一層

---

### 3. 記憶體映射佈局

SEMU 使用 MMIO (Memory-Mapped I/O) 架構，所有設備都映射到記憶體空間。

```
物理地址空間 (32-bit)
┌─────────────────────────────────────────────────────┐
│ 0x00000000                                          │
│    ↓                                                │
│ RAM (512 MB)                                        │
│    ├── 0x00000000  Kernel Image 載入點              │
│    ├── 0x1F700000  initrd (初始 RAM 磁碟, 403 MB)  │
│    └── 0x1FF00000  設備樹 DTB (511 MB)              │
│ 0x1FFFFFFF                                          │
├─────────────────────────────────────────────────────┤
│ 0x0C000000                                          │
│ ACLINT 區域                                         │
│    ├── 0x0C000000  MSWI (M-mode 軟體中斷)           │
│    ├── 0x0D000000  MTIMER (計時器)                  │
│    └── 0x0E000000  SSWI (S-mode 軟體中斷)           │
│ 0x0FFFFFFF                                          │
├─────────────────────────────────────────────────────┤
│ 0xF0000000                                          │
│ MMIO 設備區域                                       │
│    ├── 0xF0000000  PLIC (中斷控制器)                │
│    ├── 0xF4000000  UART (串口)                      │
│    ├── 0xF4100000  VirtIO-Net (網路)                │
│    ├── 0xF4200000  VirtIO-Blk (磁碟)                │
│    ├── 0xF4600000  VirtIO-RNG (隨機數)              │
│    ├── 0xF4700000  VirtIO-Snd (音效)                │
│    └── 0xF4800000  VirtIO-FS (文件系統)             │
│ 0xFFFFFFFF                                          │
└─────────────────────────────────────────────────────┘
```

**關鍵代碼**：[main.c](../main.c):600-800 的 `semu_mem_load/store` 函數

---

### 4. 頂層數據結構關係

```
emu_state_t (全局狀態)
    │
    ├── ram (512MB 記憶體)
    ├── disk (磁碟映像)
    │
    ├── vm_t (多核心容器)
    │   │
    │   └── hart_t[] (CPU 核心陣列)
    │       ├── hart[0] (主核心，啟動時運行)
    │       ├── hart[1] (次核心，SBI 啟動)
    │       └── ...
    │
    ├── 中斷控制器
    │   ├── plic_state_t (PLIC)
    │   ├── mtimer_state_t (計時器)
    │   ├── mswi_state_t (M-mode IPI)
    │   └── sswi_state_t (S-mode IPI)
    │
    ├── 外設設備
    │   ├── u8250_state_t (UART)
    │   ├── virtio_net_state_t (網路)
    │   ├── virtio_blk_state_t (磁碟)
    │   └── ... (其他 VirtIO 設備)
    │
    └── 運行時狀態
        ├── debug (除錯模式)
        └── stopped (停止標記)
```

**關鍵結構**：
- `emu_state_t`：[device.h](../device.h):428-461
- `hart_t`：[riscv.h](../riscv.h):78-170
- `vm_t`：[riscv.h](../riscv.h):172-175

---

## 🚀 系統啟動流程詳解

### 完整啟動序列

```
┌─────────────────────────────────────────────────────┐
│ 1. main() 入口                                      │
│    ├── 解析命令列參數 (-k, -i, -d, -g, -s)         │
│    └── 初始化配置結構                               │
└──────────────┬──────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────┐
│ 2. semu_init(&emu, config)                         │
│    ├── 分配 RAM (512MB)                            │
│    ├── 載入內核映像 → RAM[0x0]                     │
│    ├── 載入 initrd → RAM[403MB]                    │
│    ├── 載入設備樹 DTB → RAM[511MB]                 │
│    ├── 初始化設備                                   │
│    │   ├── UART                                     │
│    │   ├── VirtIO-Blk                               │
│    │   ├── VirtIO-Net                               │
│    │   └── ... (其他設備)                           │
│    ├── 初始化 vm_t (多核心容器)                     │
│    │   ├── 分配 hart_t[n_hart]                     │
│    │   └── 設置每個 hart 的初始狀態                 │
│    └── 設置記憶體映射回調                           │
└──────────────┬──────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────┐
│ 3. coro_init(vm, n_hart)                           │
│    └── 為每個 hart 創建協程                         │
│        ├── 分配堆疊 (8MB per hart)                  │
│        ├── 設置協程上下文                           │
│        └── 綁定執行函數                             │
└──────────────┬──────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────┐
│ 4. hart_exec_loop(state)                           │
│    └── while (!state->stopped)                     │
│        ├── coro_schedule() → 切換到可運行 hart      │
│        ├── vm_step(hart) → 執行指令                 │
│        ├── handle_exception() → 處理異常            │
│        ├── update_interrupts() → 檢查中斷           │
│        └── update_peripherals() → 更新外設狀態      │
└──────────────┬──────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────┐
│ 5. Linux Kernel Boot                               │
│    ├── 解壓內核                                     │
│    ├── SBI ecall → 設置計時器                       │
│    ├── 初始化 MMU                                   │
│    ├── SBI ecall → 啟動次級核心                     │
│    ├── 掛載 initrd 為根文件系統                     │
│    └── 執行 /init (busybox)                         │
└──────────────┬──────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────┐
│ 6. 用戶空間                                         │
│    └── Shell 提示符                                 │
└─────────────────────────────────────────────────────┘
```

---

### 關鍵代碼解析

#### 1. main() 函數

**位置**：[main.c](../main.c):1400-1537

```c
int main(int argc, char **argv) {
    // 1. 解析命令列參數
    struct {
        char *kernel_file;    // -k: 內核映像路徑
        char *initrd_file;    // -i: initrd 路徑
        char *dtb_file;       // -d: 設備樹路徑
        char *disk_file;      // -b: 磁碟映像
        bool gdb;             // -g: GDB 調試模式
        int n_hart;           // SMP 核心數（編譯時決定）
    } config;

    // 2. 創建並初始化模擬器狀態
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));

    // 3. 初始化系統
    if (!semu_init(&emu, &config)) {
        fprintf(stderr, "Initialization failed\n");
        return 1;
    }

    // 4. 啟動主循環
    hart_exec_loop(&emu);

    return 0;
}
```

**關鍵點**：
- 所有狀態集中在 `emu_state_t` 中
- 初始化失敗會立即退出
- 主循環不返回（除非停止）

---

#### 2. semu_init() 函數

**位置**：[main.c](../main.c):1200-1400

```c
bool semu_init(emu_state_t *emu, config_t *config) {
    // ══════════════════════════════════════
    // 步驟 1: 分配主記憶體 (512MB)
    // ══════════════════════════════════════
    emu->ram = mmap(NULL, RAM_SIZE,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (emu->ram == MAP_FAILED) {
        perror("mmap RAM failed");
        return false;
    }

    // ══════════════════════════════════════
    // 步驟 2: 載入內核映像到 RAM[0x0]
    // ══════════════════════════════════════
    FILE *fp = fopen(config->kernel_file, "rb");
    fseek(fp, 0, SEEK_END);
    size_t kernel_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fread(emu->ram, 1, kernel_size, fp) != kernel_size) {
        fprintf(stderr, "Failed to load kernel\n");
        return false;
    }
    fclose(fp);

    // ══════════════════════════════════════
    // 步驟 3: 載入 initrd 到高位址
    // ══════════════════════════════════════
    if (config->initrd_file) {
        uint32_t initrd_addr = 0x1F700000;  // 403MB 偏移
        fp = fopen(config->initrd_file, "rb");
        // ... 類似載入過程
    }

    // ══════════════════════════════════════
    // 步驟 4: 載入設備樹 DTB
    // ══════════════════════════════════════
    uint32_t dtb_addr = 0x1FF00000;  // 511MB 偏移
    // ... 載入 DTB

    // ══════════════════════════════════════
    // 步驟 5: 初始化 harts (CPU 核心)
    // ══════════════════════════════════════
    emu->vm.n_hart = config->n_hart;
    emu->vm.hart = calloc(config->n_hart, sizeof(hart_t *));

    for (int i = 0; i < config->n_hart; i++) {
        hart_t *hart = calloc(1, sizeof(hart_t));

        // 初始化 hart
        vm_init(hart);
        hart->mhartid = i;
        hart->priv = emu;  // 回指 emu_state_t
        hart->vm = &emu->vm;

        // 設置記憶體回調
        hart->mem_fetch = semu_mem_fetch;
        hart->mem_load = semu_mem_load;
        hart->mem_store = semu_mem_store;
        hart->mem_page_table = semu_mem_page_table;
        hart->wfi = semu_wfi;  // WFI 協程回調

        // 主核心 (hart 0) 設置初始狀態
        if (i == 0) {
            hart->pc = 0x0;  // 從 RAM 開始執行
            hart->x_regs[RV_R_A0] = i;  // hartid
            hart->x_regs[RV_R_A1] = dtb_addr;  // DTB 地址
            hart->s_mode = true;  // S-mode
            hart->hsm_status = SBI_HSM_STATE_STARTED;
        } else {
            // 次級核心設置為 STOPPED
            hart->hsm_status = SBI_HSM_STATE_STOPPED;
        }

        emu->vm.hart[i] = hart;
    }

    // ══════════════════════════════════════
    // 步驟 6: 初始化中斷控制器
    // ══════════════════════════════════════
    // ACLINT MTIMER
    emu->mtimer.mtimecmp = calloc(config->n_hart, sizeof(uint64_t));
    for (int i = 0; i < config->n_hart; i++) {
        emu->mtimer.mtimecmp[i] = UINT64_MAX;  // 初始不觸發
    }

    // MSWI / SSWI
    emu->mswi.msip = calloc(config->n_hart, sizeof(uint32_t));
    emu->sswi.ssip = calloc(config->n_hart, sizeof(uint32_t));

    // ══════════════════════════════════════
    // 步驟 7: 初始化外設設備
    // ══════════════════════════════════════

    // UART
    emu->uart.in_fd = STDIN_FILENO;
    emu->uart.out_fd = STDOUT_FILENO;
    capture_keyboard_input();  // 設置終端為 raw 模式

    // VirtIO-Blk (如果有磁碟映像)
#if SEMU_HAS(VIRTIOBLK)
    if (config->disk_file) {
        emu->disk = virtio_blk_init(&emu->vblk, config->disk_file);
        emu->vblk.ram = emu->ram;
    }
#endif

    // VirtIO-Net (網路)
#if SEMU_HAS(VIRTIONET)
    virtio_net_init(&emu->vnet, "tap0");
    emu->vnet.ram = emu->ram;
#endif

    // ... 其他設備初始化

    return true;
}
```

**關鍵點**：
1. **記憶體分配**：使用 `mmap` 分配大塊記憶體（效率高）
2. **映像載入**：直接載入到 RAM 對應位置（零拷貝）
3. **多核心初始化**：只有 hart 0 開始運行，其他處於 STOPPED 狀態
4. **回調設置**：所有記憶體訪問通過回調函數路由
5. **設備初始化**：條件編譯，根據 feature.h 決定

---

#### 3. hart_exec_loop() 主循環

**位置**：[main.c](../main.c):1100-1200

```c
void hart_exec_loop(emu_state_t *state) {
    while (!state->stopped) {
        // ═══════════════════════════════════
        // 1. 協程調度（切換到可運行的 hart）
        // ═══════════════════════════════════
        coro_schedule(&state->vm);

        // 當前 hart
        hart_t *hart = coro_current_hart(&state->vm);
        if (!hart) continue;  // 無可運行 hart

        // 跳過 STOPPED 的 hart
        if (hart->hsm_status == SBI_HSM_STATE_STOPPED)
            continue;

        // ═══════════════════════════════════
        // 2. 執行一條指令
        // ═══════════════════════════════════
        vm_step(hart);

        // ═══════════════════════════════════
        // 3. 處理異常
        // ═══════════════════════════════════
        if (hart->error == ERR_EXCEPTION) {
            // 檢查是否為 SBI ecall
            if (hart->exc_cause == RV_EXC_ECALL_S) {
                handle_sbi_ecall(hart, state);
            } else {
                // 委派給 S-mode 處理
                hart_trap(hart);
            }
        }

        // ═══════════════════════════════════
        // 4. 檢查並更新中斷
        // ═══════════════════════════════════

        // 計時器中斷
        aclint_mtimer_update_interrupts(hart, &state->mtimer);

        // 軟體中斷 (IPI)
        aclint_mswi_update_interrupts(hart, &state->mswi);
        aclint_sswi_update_interrupts(hart, &state->sswi);

        // 外部中斷 (PLIC)
        plic_update_interrupts(&state->vm, &state->plic);

        // ═══════════════════════════════════
        // 5. 週期性更新外設
        // ═══════════════════════════════════
        if (++state->peripheral_update_ctr % 100 == 0) {
            // 檢查 UART 輸入
            u8250_check_ready(&state->uart);

            // 檢查網路數據
#if SEMU_HAS(VIRTIONET)
            virtio_net_refresh_queue(&state->vnet);
#endif
        }
    }
}
```

**關鍵點**：
1. **協程調度**：使用 `coro_schedule()` 選擇下一個運行的 hart
2. **指令執行**：每次循環只執行一條指令（`vm_step`）
3. **異常處理**：區分 SBI ecall 和普通異常
4. **中斷檢查**：每條指令後檢查中斷條件
5. **外設輪詢**：定期檢查外設狀態（降低開銷）

---

### 記憶體映射實現

**位置**：[main.c](../main.c):600-800

```c
void semu_mem_load(hart_t *hart, uint32_t addr, uint8_t width, uint32_t *value) {
    emu_state_t *state = (emu_state_t *)hart->priv;

    // ═══════════════════════════════════
    // RAM 區域 (0x0 - 0x20000000)
    // ═══════════════════════════════════
    if (RANGE_CHECK(addr, 0x0, RAM_SIZE)) {
        ram_read(hart, state->ram, addr, width, value);
        return;
    }

    // ═══════════════════════════════════
    // ACLINT 區域
    // ═══════════════════════════════════

    // MSWI (0x0C000000)
    if (RANGE_CHECK(addr, MSWI_BASE, MSWI_SIZE)) {
        aclint_mswi_read(hart, &state->mswi,
                         addr - MSWI_BASE, width, value);
        return;
    }

    // MTIMER (0x0D000000)
    if (RANGE_CHECK(addr, MTIMER_BASE, MTIMER_SIZE)) {
        aclint_mtimer_read(hart, &state->mtimer,
                           addr - MTIMER_BASE, width, value);
        return;
    }

    // SSWI (0x0E000000)
    if (RANGE_CHECK(addr, SSWI_BASE, SSWI_SIZE)) {
        aclint_sswi_read(hart, &state->sswi,
                         addr - SSWI_BASE, width, value);
        return;
    }

    // ═══════════════════════════════════
    // MMIO 設備區域
    // ═══════════════════════════════════

    // PLIC (0xF0000000)
    if (RANGE_CHECK(addr, PLIC_BASE, PLIC_SIZE)) {
        plic_read(hart, &state->plic,
                  addr - PLIC_BASE, width, value);
        return;
    }

    // UART (0xF4000000)
    if (RANGE_CHECK(addr, UART_BASE, UART_SIZE)) {
        u8250_read(hart, &state->uart,
                   addr - UART_BASE, width, value);
        return;
    }

    // VirtIO-Net (0xF4100000)
#if SEMU_HAS(VIRTIONET)
    if (RANGE_CHECK(addr, VIRTIO_NET_BASE, VIRTIO_SIZE)) {
        virtio_net_read(hart, &state->vnet,
                        addr - VIRTIO_NET_BASE, width, value);
        return;
    }
#endif

    // ... 其他設備

    // 未映射區域
    vm_set_exception(hart, RV_EXC_LOAD_FAULT, addr);
}
```

**設計要點**：
1. **統一介面**：所有設備使用相同的 `read(hart, state, offset, width, value)` 介面
2. **快速路由**：使用 `RANGE_CHECK` 宏快速判斷地址範圍
3. **錯誤處理**：未映射地址觸發 Load Fault 異常
4. **條件編譯**：設備支援根據編譯選項決定

---

### 設備樹的作用

**設備樹 (Device Tree)** 告訴 Linux 內核系統中有哪些硬體設備。

**位置**：[minimal.dts](../minimal.dts)

**關鍵內容**：

```dts
/ {
    #address-cells = <1>;
    #size-cells = <1>;
    compatible = "riscv-virtio";

    // ═══════════════════════════════════
    // CPU 定義
    // ═══════════════════════════════════
    cpus {
        #address-cells = <1>;
        #size-cells = <0>;
        timebase-frequency = <10000000>;  // 10MHz

        cpu@0 {
            device_type = "cpu";
            reg = <0>;
            compatible = "riscv";
            riscv,isa = "rv32ima";  // ISA 字串
            mmu-type = "riscv,sv32";  // MMU 類型

            interrupt-controller {
                #interrupt-cells = <1>;
                interrupt-controller;
                compatible = "riscv,cpu-intc";
            };
        };
    };

    // ═══════════════════════════════════
    // 記憶體定義
    // ═══════════════════════════════════
    memory@0 {
        device_type = "memory";
        reg = <0x00000000 0x20000000>;  // 0-512MB
    };

    // ═══════════════════════════════════
    // SoC 設備
    // ═══════════════════════════════════
    soc {
        #address-cells = <1>;
        #size-cells = <1>;
        compatible = "simple-bus";
        ranges;

        // PLIC (中斷控制器)
        plic@f0000000 {
            compatible = "sifive,plic-1.0.0";
            reg = <0xf0000000 0x4000000>;
            interrupts-extended = <&cpu0_intc 11>;
            interrupt-controller;
            #interrupt-cells = <1>;
            riscv,ndev = <31>;  // 31 個中斷源
        };

        // UART
        uart@f4000000 {
            compatible = "ns16550a";
            reg = <0xf4000000 0x100>;
            interrupts = <1>;
            clock-frequency = <3686400>;
        };

        // VirtIO-Net
        virtio_net@f4100000 {
            compatible = "virtio,mmio";
            reg = <0xf4100000 0x1000>;
            interrupts = <2>;
        };

        // ... 其他設備
    };

    // ═══════════════════════════════════
    // chosen 節點（啟動參數）
    // ═══════════════════════════════════
    chosen {
        bootargs = "console=ttyS0 earlycon";
        stdout-path = "/soc/uart@f4000000";

        // initrd 位置
        linux,initrd-start = <0x1f700000>;
        linux,initrd-end = <0x1ff00000>;
    };
};
```

**Linux 內核如何使用 DTB**：

```
1. Bootloader (SEMU) 載入 DTB 到記憶體
2. 將 DTB 地址通過 a1 暫存器傳遞給內核
3. 內核解析 DTB：
   ├── 讀取 CPU 配置（核心數、ISA、timebase）
   ├── 讀取記憶體範圍
   ├── 讀取設備列表並初始化驅動程式
   └── 讀取 chosen 節點（initrd 位置、啟動參數）
```

**動態設備樹生成**：

對於多核心配置，使用 Python 腳本動態生成：

```bash
# 生成 4 核心的設備樹
./scripts/gen-hart-dts.py 4 > minimal.dts
dtc -I dts -O dtb minimal.dts -o minimal.dtb
```

---

## 🔍 實踐練習

### 練習 1：觀察啟動過程

```bash
# 編譯 SEMU
make clean && make

# 運行模擬器（顯示詳細輸出）
./build/semu -k build/Image | tee boot.log

# 觀察輸出
# - 看到 "SEMU starting" 訊息
# - 看到 Linux 內核啟動訊息
# - 看到 Shell 提示符
```

**觀察要點**：
- 內核如何解析設備樹
- 哪些設備被識別
- 啟動時間

---

### 練習 2：修改記憶體大小

**任務**：將 RAM 從 512MB 改為 256MB

**步驟**：

1. 修改 [device.h](../device.h):11
   ```c
   #define RAM_SIZE (256 * 1024 * 1024)  // 改為 256MB
   ```

2. 修改 [minimal.dts](../minimal.dts) 的 memory 節點
   ```dts
   memory@0 {
       device_type = "memory";
       reg = <0x00000000 0x10000000>;  // 256MB
   };
   ```

3. 重新編譯並運行
   ```bash
   make clean && make
   ./build/semu -k build/Image
   ```

4. 在 Linux 中檢查記憶體
   ```bash
   free -m
   cat /proc/meminfo
   ```

**預期結果**：Linux 報告的可用記憶體應該減少

---

### 練習 3：追蹤第一條指令

使用 GDB 觀察內核第一條指令的執行。

```bash
# 啟動 SEMU (GDB 模式)
./build/semu -k build/Image -g 1234 &

# 另一個終端啟動 GDB
riscv32-unknown-elf-gdb build/Image
(gdb) target remote :1234
(gdb) break *0x0
(gdb) continue

# 觀察暫存器狀態
(gdb) info registers
(gdb) x/10i $pc   # 顯示接下來 10 條指令
```

**觀察內容**：
- PC 是否為 0x0
- a0 (x10) 是否為 hartid (0)
- a1 (x11) 是否為 DTB 地址 (0x1FF00000)

---

### 練習 4：理解記憶體映射

**任務**：在運行的 Linux 中讀取 UART 寄存器

```bash
# 在 SEMU 啟動的 Linux shell 中
devmem 0xf4000000  # 讀取 UART RBR/THR 暫存器
devmem 0xf4000005  # 讀取 LSR (Line Status Register)
```

**理解流程**：
```
devmem 命令
    ↓
syscall (sys_read)
    ↓
Linux 內核 MMU 轉譯
    ↓
訪問物理地址 0xf4000000
    ↓
SEMU vm_step() 執行 LOAD 指令
    ↓
semu_mem_load() 路由到 UART
    ↓
u8250_read() 返回暫存器值
```

---

## 📊 知識檢查點

完成本階段後，你應該能回答以下問題：

### 基礎問題

1. **Q**: SEMU 的三個設計理念是什麼？
   **A**: 極簡主義、高可讀性、模組化設計

2. **Q**: 記憶體映射中，RAM 佔用哪段地址？
   **A**: 0x00000000 - 0x1FFFFFFF (512MB)

3. **Q**: 內核映像載入到哪個地址？
   **A**: 0x00000000

4. **Q**: 設備樹 DTB 載入到哪個地址？
   **A**: 0x1FF00000 (511MB 偏移)

5. **Q**: UART 映射到哪個地址？
   **A**: 0xF4000000

### 進階問題

1. **Q**: 為何使用 `mmap` 而非 `malloc` 分配 RAM？
   **A**: `mmap` 分配大塊記憶體效率更高，且可指定特殊屬性（如 MAP_ANONYMOUS）

2. **Q**: 次級核心 (hart 1, 2, ...) 如何被啟動？
   **A**: 主核心通過 SBI HSM 擴展的 `HART_START` 呼叫啟動

3. **Q**: 為何需要設備樹？
   **A**: 告訴 Linux 內核系統中有哪些硬體設備、記憶體佈局、啟動參數等

4. **Q**: `RANGE_CHECK` 宏的作用是什麼？
   **A**: 快速判斷地址是否在指定範圍內，使用位元運算優化性能

5. **Q**: 記憶體訪問如何路由到不同設備？
   **A**: 通過 `semu_mem_load/store` 函數，根據地址範圍分發到對應設備的讀寫函數

---

## 🎯 下一階段預告

完成本階段後，你已經掌握了 SEMU 的整體架構和啟動流程。

**階段 2** 將深入研究：
- RISC-V 指令解碼與執行
- MMU 虛擬記憶體轉譯
- 8×2 組相聯 TLB 快取實現
- 異常處理機制

繼續閱讀：[階段 2：CPU 核心機制](./stage2-cpu-core.md)

---

**文檔版本**：v1.0
**最後更新**：2025-01-10
**預估學習時間**：1-2 天
