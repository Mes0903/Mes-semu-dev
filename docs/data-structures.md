# SEMU 核心數據結構詳解

> 本文檔詳細說明 RISC-V 模擬器 (semu) 的核心數據結構設計與關係

---

## 📊 目錄

1. [最頂層結構：emu_state_t](#一最頂層結構emu_state_t)
2. [CPU 核心結構：hart_t](#二cpu-核心結構hart_t)
3. [多核心容器：vm_t](#三多核心容器vm_t)
4. [中斷控制器結構](#四中斷控制器結構)
5. [UART 結構](#五uart-結構)
6. [VirtIO 設備結構](#六virtio-設備結構)
7. [VirtIO-FS 特殊結構](#七virtio-fs-特殊結構)
8. [指令編碼常數](#八指令編碼常數)
9. [CSR 暫存器定義](#九csr-暫存器定義)
10. [異常代碼](#十異常代碼)
11. [SBI 擴展定義](#十一sbi-擴展定義)
12. [工具宏](#十二工具宏)
13. [核心概念總結](#核心概念總結)

---

## 一、最頂層結構：emu_state_t

**定義位置**: [device.h](../device.h):428-461

這是整個模擬器的**根狀態結構**，包含所有子系統：

```c
typedef struct {
    bool debug;              // 除錯模式開關
    bool stopped;            // 模擬器停止標記
    uint32_t *ram;           // 512MB RAM 指針
    uint32_t *disk;          // 磁碟映像指針

    vm_t vm;                 // 🔥 多核心容器（hart 陣列）
    plic_state_t plic;       // 平台中斷控制器
    u8250_state_t uart;      // UART 串口

    /* 條件編譯的 VirtIO 設備 */
    #if SEMU_HAS(VIRTIONET)
    virtio_net_state_t vnet;  // 網路卡
    #endif
    #if SEMU_HAS(VIRTIOBLK)
    virtio_blk_state_t vblk;  // 磁碟
    #endif
    #if SEMU_HAS(VIRTIORNG)
    virtio_rng_state_t vrng;  // 隨機數產生器
    #endif
    #if SEMU_HAS(VIRTIOSND)
    virtio_snd_state_t vsnd;  // 音效卡
    #endif
    #if SEMU_HAS(VIRTIOFS)
    virtio_fs_state_t vfs;    // 文件系統
    #endif

    /* ACLINT 核心本地中斷 */
    mtimer_state_t mtimer;   // 計時器
    mswi_state_t mswi;       // M-mode 軟體中斷
    sswi_state_t sswi;       // S-mode 軟體中斷

    uint32_t peripheral_update_ctr; // 外設更新計數器

    /* 除錯欄位 */
    bool is_interrupted;
    int curr_cpuid;
} emu_state_t;
```

### 記憶體佈局常數

```c
#define RAM_SIZE (512 * 1024 * 1024)  // 512MB
#define DTB_SIZE (1 * 1024 * 1024)     // 1MB（設備樹二進制）
#define INITRD_SIZE (8 * 1024 * 1024)  // 8MB（初始 RAM 磁碟）
```

---

## 二、CPU 核心結構：hart_t

**定義位置**: [riscv.h](../riscv.h):78-170

這是單個 RISC-V 硬體執行緒（hart）的完整狀態。

### 2.1 基本執行狀態

```c
struct __hart_internal {
    uint32_t x_regs[32];      // 🔥 通用暫存器 x0-x31
    uint32_t lr_reservation;  // LR/SC 原子操作保留地址（最低位=有效）
    uint32_t pc;              // 程序計數器（當前指令位置）
    uint32_t current_pc;      // 上一條開始執行的指令地址
```

**關鍵點**：
- `x_regs[0]` 永遠是 0（硬體規定，RISC-V 的 x0 寄存器）
- `current_pc`：用於異常處理時回溯指令位置

### 2.2 計時與錯誤狀態

```c
    uint64_t instret;         // 🔥 已執行指令數（用作計數器和計時器）
    semu_timer_t time;        // 時間戳
    vm_error_t error;         // 執行錯誤狀態（ERR_NONE/ERR_EXCEPTION/ERR_USER）

    /* 異常資訊（當 error = ERR_EXCEPTION 時有效） */
    uint32_t exc_cause;       // 異常原因（參考 RV_EXC_* 枚舉）
    uint32_t exc_val;         // 異常附加值（如錯誤地址）
```

**vm_error_t 枚舉** ([riscv.h](../riscv.h):28-32)：

```c
typedef enum {
    ERR_NONE,       // 正常執行
    ERR_EXCEPTION,  // RISC-V 異常（需處理 exc_cause 和 exc_val）
    ERR_USER,       // 用戶自定義錯誤（讓 vm_step() 返回）
} vm_error_t;
```

### 2.3 MMU 快取結構（🔥 最近重大更新）

```c
    mmu_fetch_cache_t cache_fetch;     // 指令抓取快取（直接映射）
    mmu_cache_set_t cache_load[8];     // 8組×2路 載入快取
    mmu_cache_set_t cache_store[8];    // 8組×2路 儲存快取
```

#### 快取結構詳解 ([riscv.h](../riscv.h):34-58)

**指令抓取快取**：儲存主機記憶體指針

```c
typedef struct {
    uint32_t n_pages;        // 虛擬頁號（VPN）
    uint32_t *page_addr;     // 🔥 主機記憶體指針（零拷貝）
#ifdef MMU_CACHE_STATS
    uint64_t hits;
    uint64_t misses;
#endif
} mmu_fetch_cache_t;
```

**載入/儲存快取**：儲存物理頁號（PPN）

```c
typedef struct {
    uint32_t n_pages;        // 虛擬頁號（VPN）
    uint32_t phys_ppn;       // 🔥 物理頁號（不是指針！）
#ifdef MMU_CACHE_STATS
    uint64_t hits;
    uint64_t misses;
#endif
} mmu_addr_cache_t;
```

**8×2 組相聯快取集合**

```c
typedef struct {
    mmu_addr_cache_t ways[2]; // 2路（每組有2個快取項）
    uint8_t lru;              // LRU 位：0 或 1（下次替換哪一路）
} mmu_cache_set_t;
```

#### 快取索引演算法（3-bit parity hash）

```c
// 實際程式碼在 riscv.c 中
uint32_t vpn = addr >> RV_PAGE_SHIFT;           // 虛擬頁號
uint8_t set_idx = __builtin_parity(vpn) & 0x7; // 3位元 parity hash
mmu_cache_set_t *set = &vm->cache_load[set_idx];
```

**設計理念**：
- **指令快取**使用直接映射（因為指令訪問具有局部性）
- **數據快取**使用組相聯（減少衝突未命中）
- **Parity hash** 分散衝突（比簡單取低位更均勻）

### 2.4 S-mode（Supervisor）狀態

```c
    /* 特權模式 */
    bool s_mode;              // true = S-mode, false = U-mode

    /* 陷阱保存的狀態 */
    bool sstatus_spp;         // 陷阱前的特權模式
    bool sstatus_spie;        // 陷阱前的中斷使能狀態
    uint32_t sepc;            // 陷阱返回地址

    /* 協程/WFI 狀態 */
    bool in_wfi;              // 🔥 是否在 WFI 狀態（等待中斷）

    /* 異常/陷阱暫存器 */
    uint32_t scause;          // 陷阱原因
    uint32_t stval;           // 陷阱附加值（錯誤地址）

    /* MMU 控制位 */
    bool sstatus_mxr;         // Make eXecutable Readable（允許讀可執行頁）
    bool sstatus_sum;         // 允許 S-mode 訪問 U-mode 頁

    /* 中斷控制 */
    bool sstatus_sie;         // S-mode 中斷使能
    uint32_t sie;             // 中斷使能遮罩（哪些中斷可觸發）
    uint32_t sip;             // 🔥 中斷待處理位（pending）

    /* 陷阱向量 */
    uint32_t stvec_addr;      // 陷阱處理程序基地址
    bool stvec_vectored;      // 向量化模式（不同異常跳不同地址）

    /* 雜項 */
    uint32_t sscratch;        // 通用暫存器（OS 常用於保存上下文指針）
    uint32_t scounteren;      // 計數器使能（U-mode 能否讀 cycle/time）

    /* MMU */
    uint32_t satp;            // 🔥 頁表基地址 + 模式（Sv32）
    uint32_t *page_table;     // 🔥 主機指針指向頁表
```

#### sip/sie 中斷位 ([riscv_private.h](../riscv_private.h):95-102)

```c
enum {
    RV_INT_SSI = 1,          // S-mode 軟體中斷
    RV_INT_SSI_BIT = (1 << RV_INT_SSI),
    RV_INT_STI = 5,          // S-mode 計時器中斷
    RV_INT_STI_BIT = (1 << RV_INT_STI),
    RV_INT_SEI = 9,          // S-mode 外部中斷
    RV_INT_SEI_BIT = (1 << RV_INT_SEI),
};
```

### 2.5 M-mode（Machine）狀態

```c
    uint32_t mhartid;         // Hart ID（多核心時的核心編號）
```

### 2.6 回調函數指針（🔥 環境提供的介面）

```c
    void *priv;               // 環境私有數據（指向 emu_state_t）

    /* WFI 協程回調 */
    void (*wfi)(hart_t *vm);  // WFI 指令觸發時呼叫（用於協程讓出）

    /* 記憶體訪問回調 */
    void (*mem_fetch)(hart_t *vm, uint32_t n_pages, uint32_t **page_addr);
    void (*mem_load)(hart_t *vm, uint32_t addr, uint8_t width, uint32_t *value);
    void (*mem_store)(hart_t *vm, uint32_t addr, uint8_t width, uint32_t value);

    /* 頁表驗證 */
    uint32_t *(*mem_page_table)(const hart_t *vm, uint32_t ppn);

    /* 多核心支援 */
    vm_t *vm;                 // 🔥 指向包含所有 harts 的 vm_t

    /* SBI HSM 擴展狀態 */
    int32_t hsm_status;       // Hart 狀態（STARTED/STOPPED/SUSPENDED）
    bool hsm_resume_is_ret;   // 恢復時是否返回（vs. 跳轉）
    int32_t hsm_resume_pc;    // 恢復執行地址
    int32_t hsm_resume_opaque;// 不透明參數
};
```

#### HSM 狀態枚舉 ([riscv_private.h](../riscv_private.h):137-145)

```c
enum {
    SBI_HSM_STATE_STARTED = 0,
    SBI_HSM_STATE_STOPPED = 1,
    SBI_HSM_STATE_START_PENDING = 2,
    SBI_HSM_STATE_STOP_PENDING = 3,
    SBI_HSM_STATE_SUSPENDED = 4,
    SBI_HSM_STATE_SUSPEND_PENDING = 5,
    SBI_HSM_STATE_RESUME_PENDING = 6
};
```

---

## 三、多核心容器：vm_t

**定義位置**: [riscv.h](../riscv.h):172-175

```c
struct __vm_internel {
    uint32_t n_hart;          // Hart 數量（SMP=N）
    hart_t **hart;            // 🔥 Hart 指針陣列
};
```

### 用法範例

```c
// 發送 IPI 到 hart 1
vm_t *vm = my_hart->vm;
hart_t *target_hart = vm->hart[1];
target_hart->sip |= RV_INT_SSI_BIT;  // 設置軟體中斷
```

---

## 四、中斷控制器結構

### 4.1 PLIC（平台級中斷控制器）

**定義位置**: [device.h](../device.h):29-35

```c
typedef struct {
    uint32_t masked;          // 已遮罩的中斷
    uint32_t ip;              // 🔥 中斷待處理位（32 中斷源）
    uint32_t ie[32];          // 中斷使能（32 源 → 32 上下文）
    uint32_t active;          // 🔥 輸入中斷線狀態（電平觸發）
} plic_state_t;
```

#### IRQ 分配 ([device.h](../device.h):50-138)

```c
#define IRQ_UART 1         // UART 串口
#define IRQ_VNET 2         // VirtIO-Net
#define IRQ_VBLK 3         // VirtIO-Blk
#define IRQ_VRNG 4         // VirtIO-RNG
#define IRQ_VSND 5         // VirtIO-Sound
#define IRQ_VFS  6         // VirtIO-FS
```

### 4.2 ACLINT MTIMER（計時器）

**定義位置**: [device.h](../device.h):229-248

```c
typedef struct {
    uint64_t *mtimecmp;       // 🔥 每個 hart 的比較值陣列
    semu_timer_t mtime;       // 當前時間（64位元）
} mtimer_state_t;
```

**工作原理**：
- 當 `mtime >= mtimecmp[hartid]` 時觸發 MTI 中斷
- Linux 內核用此實現定時器

### 4.3 ACLINT MSWI（M-mode 軟體中斷）

**定義位置**: [device.h](../device.h):263-275

```c
typedef struct {
    uint32_t *msip;           // 🔥 每個 hart 的 MSIP 暫存器
} mswi_state_t;
```

### 4.4 ACLINT SSWI（S-mode 軟體中斷）

**定義位置**: [device.h](../device.h):290-302

```c
typedef struct {
    uint32_t *ssip;           // 🔥 每個 hart 的 SSIP 暫存器
} sswi_state_t;
```

### IPI 流程

```
1. Hart 0 想喚醒 Hart 1
2. 通過 SBI ecall → sbi_send_ipi(hart_mask=0b10)
3. M-mode 設置 sswi.ssip[1] = 1
4. Hart 1 的 sip 被設置 RV_INT_SSI_BIT
5. Hart 1 觸發中斷進入陷阱處理
```

---

## 五、UART 結構

**定義位置**: [device.h](../device.h):53-66

```c
typedef struct {
    /* 8250 UART 暫存器 */
    uint8_t dll, dlh;         // 除頻器（被忽略）
    uint8_t lcr;              // 線路控制暫存器（UART 配置）
    uint8_t ier;              // 中斷使能暫存器
    uint8_t current_int;      // 當前中斷類型
    uint8_t pending_ints;     // 待處理中斷
    uint8_t mcr;              // 數據機控制（被忽略）

    /* I/O 文件描述符 */
    int in_fd, out_fd;        // 輸入/輸出 fd（通常是 stdin/stdout）
    bool in_ready;            // 是否有輸入可讀

    /* 🔥 協程支援（SMP 模式） */
    uint32_t waiting_hart_id; // 等待輸入的 Hart ID
    bool has_waiting_hart;    // 是否有 hart 在等待
} u8250_state_t;
```

### 協程式輸入處理

- Hart 嘗試讀取 UART 時，如果沒有輸入 → 協程讓出
- 當有輸入到達 → 喚醒等待的 hart

---

## 六、VirtIO 設備結構

### 6.1 VirtQueue 結構（以 virtio-blk 為例）

**定義位置**: [device.h](../device.h):140-147

```c
typedef struct {
    uint32_t QueueNum;        // 隊列大小（描述符數量）
    uint32_t QueueDesc;       // Descriptor 表地址
    uint32_t QueueAvail;      // Available 環地址
    uint32_t QueueUsed;       // Used 環地址
    uint16_t last_avail;      // 上次處理的 avail 索引
    bool ready;               // 隊列就緒標記
} virtio_blk_queue_t;
```

### VirtIO 環緩衝機制

```
Guest 提交請求：
Descriptor Table ← 放置 I/O 緩衝區描述符
        ↓
Available Ring ← 更新 idx，告知設備
        ↓
Device 處理 ← 從 last_avail 讀取請求
        ↓
Used Ring ← 完成後更新 idx
        ↓
Guest 收到中斷 ← 讀取 Used Ring
```

### 6.2 VirtIO 設備狀態

**定義位置**: [device.h](../device.h):149-165

```c
typedef struct {
    /* Feature 協商 */
    uint32_t DeviceFeaturesSel;  // Feature 選擇器（高/低 32 位）
    uint32_t DriverFeatures;     // Driver 接受的 features
    uint32_t DriverFeaturesSel;

    /* 隊列配置 */
    uint32_t QueueSel;           // 當前選擇的隊列編號
    virtio_blk_queue_t queues[2];// 2 個隊列（實際只用 1 個）

    /* 狀態與中斷 */
    uint32_t Status;             // 設備狀態（ACKNOWLEDGE/DRIVER/FEATURES_OK/DRIVER_OK）
    uint32_t InterruptStatus;    // 中斷狀態位

    /* 環境提供 */
    uint32_t *ram;               // RAM 指針（訪問 guest 記憶體）
    uint32_t *disk;              // 磁碟映像指針

    void *priv;                  // 私有數據
} virtio_blk_state_t;
```

### 6.3 VirtIO-Net 差異

**定義位置**: [device.h](../device.h):88-114

```c
typedef struct {
    // ... 相同欄位 ...
    virtio_net_queue_t queues[2];  // RX 隊列 + TX 隊列
    netdev_t peer;                 // 🔥 網路後端（TAP/vmnet/SLIRP）
    bool fd_ready;                 // 網路 fd 就緒狀態
} virtio_net_state_t;
```

---

## 七、VirtIO-FS 特殊結構

**定義位置**: [device.h](../device.h):369-408

```c
/* inode 映射表項（Guest inode ↔ 主機路徑） */
typedef struct inode_map_entry {
    uint64_t ino;                 // Guest 的 inode 編號
    char *path;                   // 主機文件系統路徑
    struct inode_map_entry *next; // 鏈表下一項
} inode_map_entry;

typedef struct {
    // ... 標準 VirtIO 欄位 ...
    virtio_fs_queue_t queues[3];  // 3 個隊列（高優先級/請求/中斷）

    char *mount_tag;              // Guest 看到的標籤（如 "myfs"）
    char *shared_dir;             // 主機共享目錄路徑

    inode_map_entry *inode_map;   // 🔥 inode 映射表（哈希表）
} virtio_fs_state_t;
```

### FUSE 協議實現

- Guest 發送 FUSE 請求（LOOKUP、READ、OPENDIR 等）
- 模擬器轉換為主機文件系統操作
- inode 映射表維護 Guest 和主機的對應關係

---

## 八、指令編碼常數

**定義位置**: [riscv_private.h](../riscv_private.h):1-17

```c
enum {
    RV32_OP_IMM    = 0b0010011,  // 立即數運算（ADDI、SLTI...）
    RV32_OP        = 0b0110011,  // 暫存器運算（ADD、SUB、SLL...）
    RV32_LUI       = 0b0110111,  // 載入立即數到高位
    RV32_AUIPC     = 0b0010111,  // 相對 PC 的立即數
    RV32_JAL       = 0b1101111,  // 無條件跳轉並連結
    RV32_JALR      = 0b1100111,  // 間接跳轉並連結
    RV32_BRANCH    = 0b1100011,  // 條件分支（BEQ、BNE...）
    RV32_LOAD      = 0b0000011,  // 載入指令
    RV32_STORE     = 0b0100011,  // 儲存指令
    RV32_MISC_MEM  = 0b0001111,  // FENCE 指令
    RV32_SYSTEM    = 0b1110011,  // 系統指令（ECALL、CSR...）
    RV32_AMO       = 0b0101111,  // 原子操作
};
```

---

## 九、CSR 暫存器定義

**定義位置**: [riscv_private.h](../riscv_private.h):42-71

### S-mode 陷阱設置

```c
RV_CSR_SSTATUS    = 0x100,  // 狀態暫存器
RV_CSR_SIE        = 0x104,  // 中斷使能
RV_CSR_STVEC      = 0x105,  // 陷阱向量基地址
RV_CSR_SCOUNTEREN = 0x106,  // 計數器使能
```

### S-mode 陷阱處理

```c
RV_CSR_SSCRATCH = 0x140,    // 暫存器
RV_CSR_SEPC     = 0x141,    // 異常程序計數器
RV_CSR_SCAUSE   = 0x142,    // 陷阱原因
RV_CSR_STVAL    = 0x143,    // 陷阱值（錯誤地址）
RV_CSR_SIP      = 0x144,    // 中斷待處理
```

### S-mode 保護與轉譯

```c
RV_CSR_SATP     = 0x180,    // 地址轉譯與保護
```

---

## 十、異常代碼

**定義位置**: [riscv_private.h](../riscv_private.h):73-90

```c
enum {
    RV_EXC_PC_MISALIGN   = 0,   // 指令地址未對齊
    RV_EXC_FETCH_FAULT   = 1,   // 指令訪問錯誤
    RV_EXC_ILLEGAL_INSN  = 2,   // 非法指令
    RV_EXC_BREAKPOINT    = 3,   // 斷點
    RV_EXC_LOAD_MISALIGN = 4,   // 載入地址未對齊
    RV_EXC_LOAD_FAULT    = 5,   // 載入訪問錯誤
    RV_EXC_STORE_MISALIGN= 6,   // 儲存地址未對齊
    RV_EXC_STORE_FAULT   = 7,   // 儲存訪問錯誤
    RV_EXC_ECALL_U       = 8,   // U-mode ecall
    RV_EXC_ECALL_S       = 9,   // S-mode ecall
    RV_EXC_FETCH_PFAULT  = 12,  // 🔥 指令頁面錯誤
    RV_EXC_LOAD_PFAULT   = 13,  // 🔥 載入頁面錯誤
    RV_EXC_STORE_PFAULT  = 15,  // 🔥 儲存頁面錯誤
};
```

---

## 十一、SBI 擴展定義

**定義位置**: [riscv_private.h](../riscv_private.h):104-158

### SBI 錯誤碼

```c
#define SBI_SUCCESS               0
#define SBI_ERR_FAILED           -1
#define SBI_ERR_NOT_SUPPORTED    -2
#define SBI_ERR_INVALID_PARAM    -3
#define SBI_ERR_DENIED           -4
#define SBI_ERR_INVALID_ADDRESS  -5
#define SBI_ERR_ALREADY_AVAILABLE -6
#define SBI_ERR_ALREADY_STARTED  -7
#define SBI_ERR_ALREADY_STOPPED  -8
#define SBI_ERR_NO_SHMEM         -9
```

### 擴展 ID

```c
#define SBI_EID_BASE    0x10        // 基礎擴展
#define SBI_EID_TIMER   0x54494D45  // 計時器（"TIME"）
#define SBI_EID_RST     0x53525354  // 重置（"RST"）
#define SBI_EID_HSM     0x48534D    // Hart 狀態管理（"HSM"）
#define SBI_EID_IPI     0x735049    // IPI（"sPI"）
#define SBI_EID_RFENCE  0x52464E43  // 遠程柵欄（"RFNC"）
```

### 常用函數 ID

```c
/* BASE */
#define SBI_BASE__GET_SBI_SPEC_VERSION 0
#define SBI_BASE__PROBE_EXTENSION 3

/* TIMER */
#define SBI_TIMER__SET_TIMER 0

/* HSM */
#define SBI_HSM__HART_START 0
#define SBI_HSM__HART_STOP 1
#define SBI_HSM__HART_GET_STATUS 2
#define SBI_HSM__HART_SUSPEND 3

/* IPI */
#define SBI_IPI__SEND_IPI 0

/* RFENCE */
#define SBI_RFENCE__I 0
#define SBI_RFENCE__VMA 1
#define SBI_RFENCE__VMA_ASID 2
```

---

## 十二、工具宏

**定義位置**: [common.h](../common.h):5-29

### 編譯器提示

```c
#define unlikely(x) __builtin_expect((x), 0)  // 分支預測（不太可能）
#define likely(x) __builtin_expect((x), 1)    // 分支預測（很可能）
```

### 位元操作

```c
#define MASK(n) (~((~0U << (n))))             // 產生 n 位元遮罩
```

### 陣列工具

```c
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))
```

### 數學工具

```c
/* 快速對數計算（向下取整的 log2） */
static inline int ilog2(int x) {
    return 31 - __builtin_clz(x | 1);
}

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
```

### 範圍檢查優化

```c
/* 使用位元運算代替雙重比較
 *
 * 傳統方式: if (x >= minx && x < minx + size)
 * 優化方式: if (RANGE_CHECK(x, minx, size))
 *
 * 原理: (x - minx) | (minx + size - 1 - x) >= 0
 * 當 x 在範圍內時，兩個減法結果的符號位都為 0
 */
#define RANGE_CHECK(x, minx, size) \
    ((int32_t) ((x - minx) | (minx + size - 1 - x)) >= 0)
```

---

## 核心概念總結

### 數據結構層次關係

```
emu_state_t (頂層)
    ├── vm_t (多核心容器)
    │   └── hart_t[] (每個核心)
    │       ├── 暫存器狀態 (x_regs, pc, CSRs)
    │       ├── MMU 快取 (8×2 組相聯)
    │       │   ├── cache_fetch (指令，直接映射)
    │       │   ├── cache_load[8] (載入，8組×2路)
    │       │   └── cache_store[8] (儲存，8組×2路)
    │       └── 回調函數 (mem_*, wfi)
    │
    ├── plic_state_t (中斷控制器)
    │   ├── ip (中斷待處理)
    │   ├── ie[32] (中斷使能)
    │   └── active (輸入線狀態)
    │
    ├── mtimer/mswi/sswi_state_t (ACLINT)
    │   ├── mtimecmp[] (計時器比較值)
    │   ├── msip[] (M-mode IPI)
    │   └── ssip[] (S-mode IPI)
    │
    ├── u8250_state_t (UART)
    │   ├── 8250 暫存器 (dll, dlh, lcr, ier)
    │   └── 協程支援 (waiting_hart_id)
    │
    └── virtio_*_state_t (VirtIO 設備)
        └── virtio_*_queue_t (VirtQueue)
            ├── QueueDesc (描述符表)
            ├── QueueAvail (可用環)
            └── QueueUsed (已用環)
```

### 記憶體映射

```
0x00000000 - 0x20000000  (512 MB)   RAM
  ├── 0x00000000          內核映像載入點
  ├── 0x1F700000          initrd 起始 (403 MB)
  └── 0x1FF00000          設備樹 (511 MB)

0xF0000000 - 0xFFFFFFFF            MMIO 區域
  ├── 0xF0000000          PLIC (64 MB)
  ├── 0xF4000000          UART (8250, 1 MB)
  ├── 0xF4100000          VirtIO-Net (1 MB)
  ├── 0xF4200000          VirtIO-Blk (1 MB)
  ├── 0xF4300000          ACLINT MTIMER (1 MB) - case 0x43 in main.c
  ├── 0xF4400000          ACLINT MSWI (1 MB)   - case 0x44 in main.c
  ├── 0xF4500000          ACLINT SSWI (1 MB)   - case 0x45 in main.c
  ├── 0xF4600000          VirtIO-RNG (1 MB)
  ├── 0xF4700000          VirtIO-Snd (1 MB)
  └── 0xF4800000          VirtIO-FS (1 MB)
```

### 中斷流程

```
設備產生中斷
    ↓
PLIC.active |= (1 << IRQ_NUM)  ← 設置中斷線
    ↓
PLIC.ip |= (1 << IRQ_NUM)      ← 更新待處理位
    ↓
if (PLIC.ie[context] & bit)    ← 檢查使能
    hart->sip |= RV_INT_SEI_BIT
    ↓
if (hart->sie & RV_INT_SEI_BIT && hart->sstatus_sie)
    觸發中斷
    ↓
hart_trap() ← 進入 S-mode 陷阱處理
    ↓
跳轉到 stvec_addr
```

### MMU 轉譯與快取

```
虛擬地址 (VA)
    ↓
計算 VPN 和 Offset
    VPN = VA >> 12
    offset = VA & 0xFFF
    ↓
查詢快取
    set_idx = __builtin_parity(VPN) & 0x7
    檢查 cache_load[set_idx].ways[0/1]
    ↓
    命中? → 返回 PPN
    ↓
    未命中 → 頁表遍歷
        ↓
        讀取 satp 獲取根頁表
        ↓
        兩級頁表遍歷（Sv32）
        ↓
        更新快取（LRU 替換）
        ↓
        返回 PPN
    ↓
物理地址 = (PPN << 12) | offset
```

### VirtIO 請求處理

```
Guest Driver
    ↓
1. 分配描述符，填充請求
    ↓
2. 更新 QueueAvail.idx
    ↓
3. 寫 QueueNotify → 觸發設備處理
    ↓
Device (semu)
    ↓
4. 讀取 last_avail
    ↓
5. 從 QueueAvail 獲取描述符索引
    ↓
6. 遍歷描述符鏈
    ↓
7. 執行 I/O 操作
    ↓
8. 更新 QueueUsed.idx
    ↓
9. 設置 InterruptStatus
    ↓
10. 觸發 PLIC 中斷
    ↓
Guest Driver 收到中斷
    ↓
11. 讀取 QueueUsed 獲取結果
```

### 重點記憶

1. **hart_t** 是核心的核心，所有執行狀態都在這裡
2. **MMU 快取**使用 3-bit parity hash 索引，8×2 組相聯
3. **中斷流程**：設備 → PLIC → hart.sip → 陷阱
4. **VirtIO** 設備共享相同模式（Feature/Queue/Status/Interrupt）
5. **回調函數**是環境與模擬器的橋樑
6. **協程式 SMP**：WFI 觸發協程讓出，事件驅動喚醒
7. **SBI ecall**：S-mode → M-mode 的系統調用介面

---

## 常見問題

### Q1: 為何指令快取存指針，而數據快取存 PPN？

**A**:
- **指令快取**：指令執行頻繁且連續，存主機指針實現零拷貝，直接訪問
- **數據快取**：數據訪問可能跨設備（RAM/MMIO），需通過 `mem_load/store` 回調處理不同區域

### Q2: 為何使用 parity hash 而非簡單取低位？

**A**:
- **簡單取低位**：相鄰頁面會映射到相鄰的 set，容易產生衝突
- **Parity hash**：計算 VPN 所有位元的 XOR，分散更均勻
- **性能**：`__builtin_parity` 在現代 CPU 上只需 1 個時鐘週期

### Q3: ACLINT 的三個組件有何區別？

**A**:
- **MTIMER**：64 位計時器，每個 hart 有獨立的 mtimecmp，用於定時中斷
- **MSWI**：M-mode 軟體中斷，SBI 用於觸發 S-mode 的 IPI
- **SSWI**：S-mode 軟體中斷，直接供 OS 使用

### Q4: VirtQueue 的三個環有何作用？

**A**:
- **Descriptor Table**：存放 I/O 緩衝區的地址和長度（共享池）
- **Available Ring**：Guest 放置待處理請求的索引（生產者）
- **Used Ring**：Device 放置已完成請求的索引（消費者）

### Q5: 為何需要 HSM 擴展？

**A**:
- 動態啟動/停止核心（省電）
- 實現 CPU 熱插拔
- 支援 hart 掛起/恢復（深度睡眠）

---

**文檔版本**: v1.0
**最後更新**: 2025-01-10
**維護者**: Mes
