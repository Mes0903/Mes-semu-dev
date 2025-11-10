# 階段 5:週邊設備系統 (Stage 5: Peripheral Device System)

本文檔深入解析 semu 的週邊設備系統,包括 UART 串口通信和完整的 VirtIO 設備框架。

## 5.1 概述 (Overview)

semu 實現了兩大類週邊設備:

```
週邊設備系統
├── UART 8250
│   ├── 終端 I/O (stdin/stdout)
│   ├── 非阻塞輪詢
│   └── Coroutine 整合
│
└── VirtIO 設備框架
    ├── VirtIO Block (磁盤)
    ├── VirtIO Network (網路)
    ├── VirtIO File System (共享目錄)
    ├── VirtIO Sound (音訊)
    └── VirtIO RNG (隨機數)
```

### 記憶體映射

```
外設記憶體佈局
┌─────────────────────────────────────┐
│ 0x10000000 - UART 8250 (8 bytes)   │ ← stdin/stdout
├─────────────────────────────────────┤
│ 0x10001000 - VirtIO Block (4KB)    │ ← 磁盤設備
├─────────────────────────────────────┤
│ 0x10002000 - VirtIO Net (4KB)      │ ← 網路設備
├─────────────────────────────────────┤
│ 0x10003000 - VirtIO FS (4KB)       │ ← 檔案系統
├─────────────────────────────────────┤
│ 0x10004000 - VirtIO Sound (4KB)    │ ← 音訊設備
├─────────────────────────────────────┤
│ 0x10005000 - VirtIO RNG (4KB)      │ ← 隨機數生成器
└─────────────────────────────────────┘
```

---

## 5.2 UART 8250 串口控制器 (UART 8250 Serial Controller)

### 5.2.1 基本架構

semu 模擬了經典的 8250 UART 控制器,負責與終端進行字元 I/O。

**數據結構** (device.h:75-91):
```c
typedef struct {
    uint8_t dll, dlh;      // Divisor Latch (波特率分頻)
    uint8_t ier;           // Interrupt Enable Register
    uint8_t lcr;           // Line Control Register (DLAB 等)
    uint8_t mcr;           // Modem Control Register
    uint8_t lsr;           // Line Status Register (狀態)

    uint8_t current_int;   // 當前中斷類型 (0-3)
    uint32_t pending_ints; // 待處理中斷 (位掩碼)

    int in_fd, out_fd;     // 輸入/輸出檔案描述符
    bool in_ready;         // 輸入是否就緒

    // SMP 模式下的 coroutine 整合
    uint32_t waiting_hart_id;
    bool has_waiting_hart;
} u8250_state_t;
```

### 5.2.2 UART 暫存器映射

8250 UART 使用 8 個字節的 MMIO 空間:

| 偏移 | DLAB=0 (讀)      | DLAB=0 (寫)      | DLAB=1          |
|------|------------------|------------------|-----------------|
| 0x0  | RBR (接收緩衝)   | THR (發送緩衝)   | DLL (分頻低位)  |
| 0x1  | IER (中斷使能)   | IER (中斷使能)   | DLH (分頻高位)  |
| 0x2  | IIR (中斷識別)   | FCR (FIFO 控制)  | —               |
| 0x3  | LCR (線路控制)   | LCR (線路控制)   | —               |
| 0x4  | MCR (數據機控制) | MCR (數據機控制) | —               |
| 0x5  | LSR (線路狀態)   | —                | —               |
| 0x6  | MSR (數據機狀態) | —                | —               |
| 0x7  | (無刮擦暫存器)   | —                | —               |

### 5.2.3 中斷機制

**中斷優先級** (uart.c:33):
```c
#define U8250_INT_THRE 1  // Transmit Holding Register Empty
```

**中斷更新邏輯** (uart.c:56-71):
```c
void u8250_update_interrupts(u8250_state_t *uart)
{
    // 1. 處理電平觸發的中斷
    if (uart->in_ready)
        uart->pending_ints |= 1;   // 接收數據就緒
    else
        uart->pending_ints &= ~1;  // 清除接收中斷

    // 2. 只保留已使能的中斷
    uart->pending_ints &= uart->ier;

    // 3. 更新當前中斷 (高位 = 高優先級)
    if (uart->pending_ints)
        uart->current_int = ilog2(uart->pending_ints);
}
```

### 5.2.4 終端模式管理

**捕獲鍵盤輸入** (uart.c:44-54):
```c
void capture_keyboard_input()
{
    // 註冊退出時恢復終端的回調
    atexit(reset_keyboard_input);

    struct termios term;
    tcgetattr(0, &term);

    // 關閉標準模式、回顯和訊號
    term.c_lflag &= ~(ICANON | ECHO | ISIG);

    tcsetattr(0, TCSANOW, &term);
}
```

這使得 semu 能夠捕獲每一個按鍵而不需要等待回車,並且不會回顯到終端。

**退出熱鍵**: `Ctrl-a x` - 立即終止模擬器 (uart.c:135-140)

### 5.2.5 Coroutine 整合:等待輸入

在 SMP 模式下,當沒有 UART 輸入時,hart 會讓出 CPU 而不是忙等待:

**等待輸入實現** (uart.c:95-114):
```c
static void u8250_wait_for_input(u8250_state_t *uart)
{
    // 1. 檢查是否在 coroutine 中執行
    uint32_t hart_id = coro_current_hart_id();
    if (hart_id == UINT32_MAX)
        return;  // 不在 coroutine 中,跳過讓出

    // 2. 標記此 hart 正在等待 UART 輸入
    uart->waiting_hart_id = hart_id;
    uart->has_waiting_hart = true;

    // 3. 讓出 CPU,等待事件循環喚醒
    coro_yield();

    // 4. 恢復執行 - 清除等待狀態
    uart->has_waiting_hart = false;
    uart->waiting_hart_id = UINT32_MAX;
}
```

**讀取邏輯** (uart.c:116-143):
```c
static uint8_t u8250_handle_in(u8250_state_t *uart)
{
    uint8_t value = 0;
    u8250_check_ready(uart);  // 檢查是否有數據可讀

    // 如果沒有數據,讓出 CPU 並等待
    if (!uart->in_ready) {
        u8250_wait_for_input(uart);

        // 恢復後重新檢查
        u8250_check_ready(uart);
        if (!uart->in_ready)
            return value;  // 偽喚醒
    }

    // 讀取一個字節
    if (read(uart->in_fd, &value, 1) < 0)
        fprintf(stderr, "failed to read UART input: %s\n", strerror(errno));

    uart->in_ready = false;
    u8250_check_ready(uart);  // 檢查是否還有更多數據

    // 特殊退出序列: Ctrl-a x
    if (value == 1) {
        if (getchar() == 120) {
            printf("\n");
            exit(0);
        }
    }

    return value;
}
```

### 5.2.6 暫存器讀寫

**讀取暫存器** (uart.c:145-185):
```c
static void u8250_reg_read(u8250_state_t *uart, uint32_t addr, uint8_t *value)
{
    switch (addr) {
    case 0:  // RBR/DLL
        if (uart->lcr & (1 << 7))  // DLAB 位元
            *value = uart->dll;
        else
            *value = u8250_handle_in(uart);  // 讀取輸入
        break;

    case 1:  // IER/DLH
        *value = (uart->lcr & 0x80) ? uart->dlh : uart->ier;
        break;

    case 2:  // IIR
        *value = (uart->current_int << 1) | (uart->pending_ints ? 0 : 1);
        if (uart->current_int == U8250_INT_THRE)
            uart->pending_ints &= ~(1 << uart->current_int);  // 清除 THRE
        break;

    case 3: *value = uart->lcr; break;
    case 4: *value = uart->mcr; break;

    case 5:  // LSR
        *value = 0x60 | (uint8_t) uart->in_ready;  // TX 完成 + 輸入就緒
        break;

    case 6:  // MSR
        *value = 0xb0;  // 載波檢測、數據就緒、清除發送
        break;

    default:
        *value = 0;
    }
}
```

**寫入暫存器** (uart.c:187-212):
```c
static void u8250_reg_write(u8250_state_t *uart, uint32_t addr, uint8_t value)
{
    switch (addr) {
    case 0:  // THR/DLL
        if (uart->lcr & (1 << 7))
            uart->dll = value;
        else {
            u8250_handle_out(uart, value);  // 輸出字元
            uart->pending_ints |= 1 << U8250_INT_THRE;  // 設置 THRE 中斷
        }
        break;

    case 1:  // IER/DLH
        if (uart->lcr & 0x80)
            uart->dlh = value;
        else
            uart->ier = value;
        break;

    case 3: uart->lcr = value; break;
    case 4: uart->mcr = value; break;
    }
}
```

---

## 5.3 VirtIO 設備框架 (VirtIO Device Framework)

### 5.3.1 VirtIO 概述

VirtIO 是專為虛擬化環境設計的 I/O 標準,具有以下優勢:

- **統一接口**: 所有設備共享相同的 MMIO 暫存器佈局
- **高效率**: 批量處理和零拷貝 I/O
- **可移植性**: 跨平台標準

### 5.3.2 VirtIO MMIO 暫存器佈局

所有 VirtIO 設備使用相同的 MMIO 暫存器映射 (virtio.h:74-99):

```
VirtIO MMIO 暫存器 (相對偏移)
┌──────────────────────────┬──────┬──────┬────────────────────────┐
│ 暫存器名稱               │ 偏移 │ 訪問 │ 用途                   │
├──────────────────────────┼──────┼──────┼────────────────────────┤
│ MagicValue               │ 0x00 │  R   │ 魔術值 0x74726976      │
│ Version                  │ 0x04 │  R   │ 版本 (2)               │
│ DeviceID                 │ 0x08 │  R   │ 設備 ID (1=net, 2=blk) │
│ VendorID                 │ 0x0c │  R   │ 廠商 ID                │
│ DeviceFeatures           │ 0x10 │  R   │ 設備特性 (當前選擇頁)  │
│ DeviceFeaturesSel        │ 0x14 │  W   │ 選擇特性頁 (0/1)       │
│ DriverFeatures           │ 0x20 │  W   │ 驅動啟用的特性         │
│ DriverFeaturesSel        │ 0x24 │  W   │ 選擇驅動特性頁         │
│ QueueSel                 │ 0x30 │  W   │ 選擇隊列索引           │
│ QueueNumMax              │ 0x34 │  R   │ 最大隊列大小           │
│ QueueNum                 │ 0x38 │  W   │ 隊列大小               │
│ QueueReady               │ 0x44 │ RW   │ 隊列就緒標誌           │
│ QueueNotify              │ 0x50 │  W   │ 通知設備處理隊列       │
│ InterruptStatus          │ 0x60 │  R   │ 中斷狀態               │
│ InterruptACK             │ 0x64 │  W   │ 確認中斷               │
│ Status                   │ 0x70 │ RW   │ 設備狀態               │
│ QueueDescLow             │ 0x80 │  W   │ 描述符表地址 (低)      │
│ QueueDescHigh            │ 0x84 │  W   │ 描述符表地址 (高)      │
│ QueueDriverLow           │ 0x90 │  W   │ 可用隊列地址 (低)      │
│ QueueDriverHigh          │ 0x94 │  W   │ 可用隊列地址 (高)      │
│ QueueDeviceLow           │ 0xa0 │  W   │ 已用隊列地址 (低)      │
│ QueueDeviceHigh          │ 0xa4 │  W   │ 已用隊列地址 (高)      │
│ ConfigGeneration         │ 0xfc │  R   │ 配置代數               │
│ Config                   │ 0x100│ RW   │ 設備專屬配置 (256B)    │
└──────────────────────────┴──────┴──────┴────────────────────────┘
```

### 5.3.3 VirtIO 隊列結構 (Virtqueue)

VirtIO 使用三個環形緩衝區進行通信:

```
Virtqueue 結構 (Split Virtqueue)
┌────────────────────────────────────────────────┐
│ 1. 描述符表 (Descriptor Table)                │
│    ┌──────────────────────────────────┐       │
│    │ desc[0]: addr, len, flags, next  │       │
│    │ desc[1]: addr, len, flags, next  │       │
│    │ ...                              │       │
│    │ desc[N-1]                        │       │
│    └──────────────────────────────────┘       │
│                                                │
│ 2. 可用環 (Available Ring) - 驅動 → 設備     │
│    ┌──────────────────────────────────┐       │
│    │ flags  (2 bytes)                 │       │
│    │ idx    (2 bytes) ← 寫入位置      │       │
│    │ ring[0..N-1] (描述符鏈頭索引)    │       │
│    │ used_event (可選)                │       │
│    └──────────────────────────────────┘       │
│                                                │
│ 3. 已用環 (Used Ring) - 設備 → 驅動          │
│    ┌──────────────────────────────────┐       │
│    │ flags  (2 bytes)                 │       │
│    │ idx    (2 bytes) ← 已處理位置    │       │
│    │ ring[0]: id, len (描述符 + 長度) │       │
│    │ ring[1]: id, len                 │       │
│    │ ...                              │       │
│    │ avail_event (可選)               │       │
│    └──────────────────────────────────┘       │
└────────────────────────────────────────────────┘
```

**描述符結構** (virtio.h:107-112):
```c
struct virtq_desc {
    uint64_t addr;    // 物理地址 (guest RAM 偏移)
    uint32_t len;     // 緩衝區長度
    uint16_t flags;   // NEXT, WRITE 標誌
    uint16_t next;    // 下一個描述符索引 (若 NEXT 設置)
};

// 標誌位
#define VIRTIO_DESC_F_NEXT  1  // 有下一個描述符
#define VIRTIO_DESC_F_WRITE 2  // 設備可寫 (否則只讀)
```

### 5.3.4 VirtIO 狀態機

**設備狀態** (virtio.h:5-6):
```c
#define VIRTIO_STATUS__DRIVER_OK          4   // 驅動初始化完成
#define VIRTIO_STATUS__DEVICE_NEEDS_RESET 64  // 設備錯誤,需重置
```

**中斷類型** (virtio.h:8-9):
```c
#define VIRTIO_INT__USED_RING  1  // 已用環更新
#define VIRTIO_INT__CONF_CHANGE 2  // 配置變更
```

---

## 5.4 VirtIO Block 設備 (VirtIO Block Device)

### 5.4.1 磁盤設備概述

VirtIO Block 提供塊設備接口,用於模擬磁盤驅動器。

**配置結構** (virtio-blk.c:29-58):
```c
struct virtio_blk_config {
    uint64_t capacity;        // 磁盤容量 (扇區數)
    uint32_t size_max;        // 最大段大小
    uint32_t seg_max;         // 最大段數量

    struct virtio_blk_geometry {
        uint16_t cylinders;   // CHS 幾何參數
        uint8_t heads;
        uint8_t sectors;
    } geometry;

    uint32_t blk_size;        // 塊大小 (字節)

    struct virtio_blk_topology {
        uint8_t physical_block_exp;
        uint8_t alignment_offset;
        uint16_t min_io_size;
        uint32_t opt_io_size;
    } topology;

    // ... 其他高級特性
};
```

### 5.4.2 磁盤初始化

**初始化流程** (virtio-blk.c:435-481):
```c
uint32_t *virtio_blk_init(virtio_blk_state_t *vblk, char *disk_file)
{
    // 1. 分配配置結構
    vblk->priv = &vblk_configs[vblk_dev_cnt++];

    // 2. 如果沒有磁盤映像,設置容量為 0
    if (!disk_file) {
        PRIV(vblk)->capacity = 0;
        return NULL;
    }

    // 3. 打開磁盤文件
    int disk_fd = open(disk_file, O_RDWR);
    if (disk_fd < 0) {
        fprintf(stderr, "could not open %s\n", disk_file);
        exit(2);
    }

    // 4. 獲取文件大小
    struct stat st;
    fstat(disk_fd, &st);
    size_t disk_size = st.st_size;

    // 5. 將磁盤映射到記憶體 (零拷貝)
    uint32_t *disk_mem = mmap(NULL, disk_size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, disk_fd, 0);
    if (disk_mem == MAP_FAILED) {
        fprintf(stderr, "Could not map disk\n");
        return NULL;
    }
    close(disk_fd);

    // 6. 設置設備狀態
    vblk->disk = disk_mem;
    PRIV(vblk)->capacity = (disk_size - 1) / DISK_BLK_SIZE + 1;

    return disk_mem;
}
```

**記憶體映射優勢**:
- **零拷貝**: 直接在映射的記憶體上操作
- **自動同步**: 寫入自動持久化到磁盤文件
- **高效**: 避免 read/write 系統調用開銷

### 5.4.3 塊請求處理

VirtIO Block 使用 3 個描述符表示一個完整的請求:

```
塊請求佈局
┌─────────────────────────────────────┐
│ desc[0] (device-readable):          │
│   ┌─────────────────────────────┐  │
│   │ uint32_t type (IN/OUT/FLUSH)│  │ ← 請求頭
│   │ uint32_t reserved           │  │
│   │ uint64_t sector (起始扇區)  │  │
│   └─────────────────────────────┘  │
│         ↓ (flags & NEXT)            │
│ desc[1] (視類型決定方向):           │
│   ┌─────────────────────────────┐  │
│   │ uint8_t data[512*N]         │  │ ← 數據緩衝區
│   └─────────────────────────────┘  │
│         ↓ (flags & NEXT)            │
│ desc[2] (device-writable):          │
│   ┌─────────────────────────────┐  │
│   │ uint8_t status (0=OK)       │  │ ← 狀態
│   └─────────────────────────────┘  │
└─────────────────────────────────────┘
```

**描述符鏈處理** (virtio-blk.c:124-199):
```c
static int virtio_blk_desc_handler(virtio_blk_state_t *vblk,
                                   const virtio_blk_queue_t *queue,
                                   uint32_t desc_idx,
                                   uint32_t *plen)
{
    struct virtq_desc vq_desc[3];

    // 1. 收集描述符鏈 (3 個描述符)
    for (int i = 0; i < 3; i++) {
        const struct virtq_desc *desc =
            (struct virtq_desc *) &vblk->ram[queue->QueueDesc + desc_idx * 4];

        vq_desc[i].addr = desc->addr;
        vq_desc[i].len = desc->len;
        vq_desc[i].flags = desc->flags;
        desc_idx = desc->next;
    }

    // 2. 驗證描述符鏈 (前兩個應有 NEXT 標誌)
    if (!(vq_desc[0].flags & VIRTIO_DESC_F_NEXT) ||
        !(vq_desc[1].flags & VIRTIO_DESC_F_NEXT) ||
        (vq_desc[2].flags & VIRTIO_DESC_F_NEXT)) {
        virtio_blk_set_fail(vblk);
        return -1;
    }

    // 3. 解析請求頭
    const struct vblk_req_header *header =
        (struct vblk_req_header *) ((uintptr_t) vblk->ram + vq_desc[0].addr);
    uint32_t type = header->type;
    uint64_t sector = header->sector;
    uint8_t *status = (uint8_t *) ((uintptr_t) vblk->ram + vq_desc[2].addr);

    // 4. 檢查扇區索引
    if (sector > (PRIV(vblk)->capacity - 1)) {
        *status = VIRTIO_BLK_S_IOERR;
        return -1;
    }

    // 5. 處理數據
    switch (type) {
    case VIRTIO_BLK_T_IN:  // 讀取
        virtio_blk_read_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    case VIRTIO_BLK_T_OUT:  // 寫入
        virtio_blk_write_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    default:
        *status = VIRTIO_BLK_S_UNSUPP;
        return -1;
    }

    // 6. 返回成功狀態
    *status = VIRTIO_BLK_S_OK;
    *plen = vq_desc[1].len;

    return 0;
}
```

**讀寫實現** (virtio-blk.c:103-122):
```c
// 寫入磁盤
static void virtio_blk_write_handler(virtio_blk_state_t *vblk,
                                     uint64_t sector,
                                     uint64_t desc_addr,
                                     uint32_t len)
{
    void *dest = (void *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    const void *src = (void *) ((uintptr_t) vblk->ram + desc_addr);
    memcpy(dest, src, len);  // 零拷貝:直接複製到 mmap 區域
}

// 從磁盤讀取
static void virtio_blk_read_handler(virtio_blk_state_t *vblk,
                                    uint64_t sector,
                                    uint64_t desc_addr,
                                    uint32_t len)
{
    void *dest = (void *) ((uintptr_t) vblk->ram + desc_addr);
    const void *src = (void *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    memcpy(dest, src, len);
}
```

### 5.4.4 隊列通知處理

當驅動寫入 `QueueNotify` 暫存器時觸發:

**隊列處理流程** (virtio-blk.c:201-260):
```c
static void virtio_queue_notify_handler(virtio_blk_state_t *vblk, int index)
{
    uint32_t *ram = vblk->ram;
    virtio_blk_queue_t *queue = &vblk->queues[index];

    // 1. 檢查設備和隊列狀態
    if (vblk->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;
    if (!((vblk->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        return virtio_blk_set_fail(vblk);

    // 2. 檢查可用隊列是否有新項目
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;  // virtq_avail.idx
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)
        return virtio_blk_set_fail(vblk);

    if (queue->last_avail == new_avail)
        return;  // 沒有新請求

    // 3. 處理所有可用的請求
    uint16_t new_used = ram[queue->QueueUsed] >> 16;
    while (queue->last_avail != new_avail) {
        // 3.1 計算隊列中的索引 (循環緩衝區)
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;

        // 3.2 從可用隊列讀取描述符索引
        // 佈局: ring[queue_idx] 是 2 字節,但記憶體對齊到 4 字節
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        // 3.3 處理描述符鏈
        uint32_t len = 0;
        int result = virtio_blk_desc_handler(vblk, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_blk_set_fail(vblk);

        // 3.4 將結果寫入已用隊列
        uint32_t vq_used_addr = queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;  // virtq_used_elem.id
        ram[vq_used_addr + 1] = len;     // virtq_used_elem.len

        queue->last_avail++;
        new_used++;
    }

    // 4. 更新已用隊列的 idx
    vblk->ram[queue->QueueUsed] &= MASK(16);
    vblk->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16;

    // 5. 發送中斷 (除非驅動禁用)
    if (!(ram[queue->QueueAvail] & 1))  // VIRTQ_AVAIL_F_NO_INTERRUPT
        vblk->InterruptStatus |= VIRTIO_INT__USED_RING;
}
```

---

## 5.5 VirtIO Network 設備 (VirtIO Network Device)

### 5.5.1 網路設備概述

VirtIO Network 提供網路適配器功能,支持三種後端:

1. **TAP** (Linux): 內核網路橋接
2. **vmnet** (macOS): Apple 的網路框架
3. **User mode (slirp)**: 用戶空間網路棧

**配置結構** (virtio-net.c:36-41):
```c
struct virtio_net_config {
    uint8_t mac[6];               // MAC 地址
    uint16_t status;              // 鏈路狀態
    uint16_t max_virtqueue_pairs; // 多隊列支持
    uint16_t mtu;                 // 最大傳輸單元
};
```

### 5.5.2 隊列結構

VirtIO Net 使用兩個隊列:

```
網路隊列配置
┌─────────────────────────────────────┐
│ Queue 0: RX (接收)                  │
│   ← 從網路後端讀取封包              │
│   ← 驅動提供緩衝區                  │
│                                     │
│ Queue 1: TX (發送)                  │
│   → 驅動提供封包數據                │
│   → 寫入網路後端                    │
└─────────────────────────────────────┘
```

### 5.5.3 VirtIO Net Header

每個封包都帶有 12 字節的 VirtIO 頭部:

```c
struct virtio_net_hdr {
    uint8_t flags;        // 標誌 (如 NEEDS_CSUM)
    uint8_t gso_type;     // GSO 類型
    uint16_t hdr_len;     // 頭部長度
    uint16_t gso_size;    // GSO 段大小
    uint16_t csum_start;  // 校驗和起始偏移
    uint16_t csum_offset; // 校驗和偏移
    uint16_t num_buffers; // 緩衝區數量 (合併 RX)
};
```

在 semu 的簡化實現中 (virtio-net.c:322-327):
```c
// 接收封包時:設置 num_buffers = 1
uint8_t virtio_header[12];
memset(virtio_header, 0, sizeof(virtio_header));
virtio_header[10] = 1;  // num_buffers = 1

// 發送封包時:跳過 12 字節頭部
vnet_iovec_read(&buffer_iovs_cursor, &buffer_niovs,
                virtio_header, sizeof(virtio_header));
```

### 5.5.4 描述符緩衝區處理

**零拷貝 I/O 向量處理**:

VirtIO Net 使用 `iovec` 結構避免中間緩衝區:

```c
struct iovec {
    void *iov_base;   // 緩衝區起始地址
    size_t iov_len;   // 緩衝區長度
};
```

**描述符鏈轉換為 iovec** (virtio-net.c:277-292 宏):
```c
#define VNET_BUFFER_TO_IOV(expect_readable)                            \
    uint16_t desc_idx;                                                 \
    /* 第一遍:驗證標誌並計數 */                                        \
    size_t buffer_niovs = 0;                                           \
    VNET_ITERATE_BUFFER(                                               \
        true,                                                          \
        if ((!!(desc_flags & VIRTIO_DESC_F_WRITE)) != (expect_readable)) \
            return virtio_net_set_fail(vnet);                          \
        buffer_niovs++;                                                \
    )                                                                  \
    /* 轉換為 iov */                                                   \
    struct iovec buffer_iovs[buffer_niovs];                            \
    buffer_niovs = 0;                                                  \
    VNET_ITERATE_BUFFER(                                               \
        false,                                                         \
        uint64_t desc_addr = desc->addr;                               \
        uint32_t desc_len = desc->len;                                 \
        buffer_iovs[buffer_niovs].iov_base =                           \
            (void *) ((uintptr_t) ram + desc_addr);  /* 直接指向 guest RAM */ \
        buffer_iovs[buffer_niovs].iov_len = desc_len;                  \
        buffer_niovs++;                                                \
    )
```

### 5.5.5 網路後端接口

**讀取封包** (virtio-net.c:119-191):
```c
static ssize_t handle_read(netdev_t *netdev,
                           virtio_net_queue_t *queue,
                           struct iovec *iovs_cursor,
                           size_t niovs)
{
    ssize_t plen = 0;
    switch (netdev->type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet: {
        // macOS vmnet: 讀取到臨時緩衝區再複製
        net_vmnet_state_t *vmnet = (net_vmnet_state_t *) netdev->op;
        uint8_t buf[2048];

        plen = net_vmnet_read(vmnet, buf, sizeof(buf));
        if (plen < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            queue->fd_ready = false;
            return -1;
        }

        // 複製到 iovec
        struct iovec *vecs = iovs_cursor;
        size_t nvecs = niovs;
        if (vnet_iovec_write(&vecs, &nvecs, buf, plen)) {
            fprintf(stderr, "[VNET] packet too large for iovec\n");
            return -1;
        }
        break;
    }
#else
    case NETDEV_IMPL_tap: {
        // Linux TAP: 直接零拷貝讀取到 iovec
        net_tap_options_t *tap = (net_tap_options_t *) netdev->op;
        plen = readv(tap->tap_fd, iovs_cursor, niovs);
        if (plen < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            queue->fd_ready = false;
            return -1;
        }
        break;
    }
#endif
    case NETDEV_IMPL_user: {
        // slirp 用戶模式
        net_user_options_t *usr = (net_user_options_t *) netdev->op;
        plen = readv(usr->guest_to_host_channel[SLIRP_READ_SIDE],
                     iovs_cursor, niovs);
        if (plen < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            queue->fd_ready = false;
            return -1;
        }
        break;
    }
    }
    return plen;
}
```

**寫入封包** (virtio-net.c:193-257):
```c
static ssize_t handle_write(netdev_t *netdev,
                            virtio_net_queue_t *queue,
                            struct iovec *iovs_cursor,
                            size_t niovs)
{
    ssize_t plen = 0;
    switch (netdev->type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet: {
        // macOS vmnet: 使用零拷貝 writev
        net_vmnet_state_t *vmnet = (net_vmnet_state_t *) netdev->op;
        ssize_t written = net_vmnet_writev(vmnet, iovs_cursor, niovs);
        if (written < 0) {
            queue->fd_ready = false;
            return -1;
        }
        plen = written;
        break;
    }
#else
    case NETDEV_IMPL_tap: {
        // Linux TAP: 直接零拷貝寫入
        net_tap_options_t *tap = (net_tap_options_t *) netdev->op;
        plen = writev(tap->tap_fd, iovs_cursor, niovs);
        if (plen < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            queue->fd_ready = false;
            return -1;
        }
        break;
    }
#endif
    case NETDEV_IMPL_user: {
        // slirp: 需要聚合到連續緩衝區
        net_user_options_t *usr = (net_user_options_t *) netdev->op;
        uint8_t pkt[1514];

        for (size_t i = 0; i < niovs; i++) {
            memcpy(pkt + plen, iovs_cursor[i].iov_base, iovs_cursor[i].iov_len);
            plen += iovs_cursor[i].iov_len;
        }

        ssize_t written = write(usr->host_to_guest_channel[SLIRP_WRITE_SIDE],
                                pkt, plen);
        if (written < 0) {
            queue->fd_ready = false;
            return -1;
        }
        plen = written;
        break;
    }
    }
    return plen;
}
```

### 5.5.6 隊列處理宏生成

semu 使用宏生成 RX 和 TX 隊列處理函數 (virtio-net.c:294-351):

```c
#define VNET_GENERATE_QUEUE_HANDLER(NAME_SUFFIX, VERB, QUEUE_IDX, READ)  \
    static void virtio_net_try_##NAME_SUFFIX(virtio_net_state_t *vnet)   \
    {                                                                     \
        uint32_t *ram = vnet->ram;                                        \
        virtio_net_queue_t *queue = &vnet->queues[QUEUE_IDX];            \

        /* ... 檢查設備狀態 ... */

        /* 檢查可用隊列 */
        uint16_t new_avail = ram[queue->QueueAvail] >> 16;               \
        /* ... */

        /* 處理所有可用請求 */
        uint16_t new_used = ram[queue->QueueUsed] >> 16;                 \
        while (queue->last_avail != new_avail) {                          \
            /* 獲取描述符索引 */
            uint16_t queue_idx = queue->last_avail % queue->QueueNum;    \
            uint16_t buffer_idx =                                         \
                ram[queue->QueueAvail + 1 + queue_idx / 2] >>            \
                (16 * (queue_idx % 2));                                   \

            /* 轉換描述符為 iovec */
            VNET_BUFFER_TO_IOV(READ)                                      \
            struct iovec *buffer_iovs_cursor = buffer_iovs;               \

            /* 處理 VirtIO 頭部 */
            uint8_t virtio_header[12];                                    \
            if (READ) {  /* RX: 寫入頭部 */
                memset(virtio_header, 0, sizeof(virtio_header));          \
                virtio_header[10] = 1;  /* num_buffers */
                vnet_iovec_write(&buffer_iovs_cursor, &buffer_niovs,      \
                                 virtio_header, sizeof(virtio_header));   \
            } else {  /* TX: 跳過頭部 */
                vnet_iovec_read(&buffer_iovs_cursor, &buffer_niovs,       \
                                virtio_header, sizeof(virtio_header));    \
            }                                                             \

            /* 執行實際的讀/寫操作 */
            ssize_t plen = handle_##VERB(&vnet->peer, queue,              \
                                         buffer_iovs_cursor, buffer_niovs); \
            if (plen < 0)                                                 \
                break;  /* 沒有更多封包 */

            /* 寫入已用隊列 */
            queue->last_avail++;                                          \
            ram[queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2] = \
                buffer_idx;                                               \
            ram[queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2 + 1] = \
                READ ? (plen + sizeof(virtio_header)) : 0;  /* RX 返回長度,TX 返回 0 */ \
            new_used++;                                                   \
        }                                                                 \

        /* 更新已用隊列索引 */
        vnet->ram[queue->QueueUsed] &= MASK(16);                          \
        vnet->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16;      \

        /* 發送中斷 */
        if (!(ram[queue->QueueAvail] & 1))                                \
            vnet->InterruptStatus |= VIRTIO_INT__USED_RING;               \
    }

// 生成兩個函數:
VNET_GENERATE_QUEUE_HANDLER(rx, read, VNET_QUEUE_RX, true)
VNET_GENERATE_QUEUE_HANDLER(tx, write, VNET_QUEUE_TX, false)
```

### 5.5.7 隊列刷新機制

**主循環輪詢** (virtio-net.c:356-424):
```c
void virtio_net_refresh_queue(virtio_net_state_t *vnet)
{
    if (!(vnet->Status & VIRTIO_STATUS__DRIVER_OK) ||
        (vnet->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET))
        return;

    if (!vnet->peer.op)
        return;  // 網路後端未初始化

    netdev_impl_t dev_type = vnet->peer.type;
    switch (dev_type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet: {
        net_vmnet_state_t *vmnet = (net_vmnet_state_t *) vnet->peer.op;
        int fd = net_vmnet_get_fd(vmnet);

        // 檢查是否有封包可讀
        struct pollfd pfd = {fd, POLLIN, 0};
        poll(&pfd, 1, 0);
        if (pfd.revents & POLLIN) {
            vnet->queues[VNET_QUEUE_RX].fd_ready = true;
            virtio_net_try_rx(vnet);  // 處理接收
        }

        // vmnet 異步寫入,TX 隊列總是就緒
        vnet->queues[VNET_QUEUE_TX].fd_ready = true;
        virtio_net_try_tx(vnet);
        break;
    }
#else
    case NETDEV_IMPL_tap: {
        net_tap_options_t *tap = (net_tap_options_t *) vnet->peer.op;

        // 檢查讀/寫就緒狀態
        struct pollfd pfd = {tap->tap_fd, POLLIN | POLLOUT, 0};
        poll(&pfd, 1, 0);

        if (pfd.revents & POLLIN) {
            vnet->queues[VNET_QUEUE_RX].fd_ready = true;
            virtio_net_try_rx(vnet);
        }
        if (pfd.revents & POLLOUT) {
            vnet->queues[VNET_QUEUE_TX].fd_ready = true;
            virtio_net_try_tx(vnet);
        }
        break;
    }
#endif
    case NETDEV_IMPL_user: {
        net_user_options_t *usr = (net_user_options_t *) vnet->peer.op;

        // 三個文件描述符:guest→host, host→guest (read), host→guest (write)
        struct pollfd pfd[3] = {
            {usr->guest_to_host_channel[SLIRP_READ_SIDE], POLLIN, 0},
            {usr->host_to_guest_channel[SLIRP_READ_SIDE], POLLIN, 0},
            {usr->host_to_guest_channel[SLIRP_WRITE_SIDE], POLLOUT, 0}
        };
        poll(pfd, 3, 0);

        if (pfd[0].revents & POLLIN) {
            vnet->queues[VNET_QUEUE_RX].fd_ready = true;
            virtio_net_try_rx(vnet);
        }
        if (pfd[1].revents & POLLIN) {
            net_slirp_read(usr);  // 處理 slirp 內部封包
        }
        if (pfd[2].revents & POLLOUT) {
            vnet->queues[VNET_QUEUE_TX].fd_ready = true;
            virtio_net_try_tx(vnet);
        }
        break;
    }
    }
}
```

這個函數在主循環中被持續調用 (在 `main.c` 的 VM 執行循環中),確保網路封包能及時處理。

---

## 5.6 VirtIO File System (virtiofs)

### 5.6.1 VirtIO-FS 概述

VirtIO-FS 實現了 FUSE (Filesystem in Userspace) 協議,允許 guest 掛載 host 的目錄。

**配置結構** (virtio-fs.c:28-32):
```c
struct virtio_fs_config {
    char tag[36];                // 掛載標籤 (guest 用於識別)
    uint32_t num_request_queues; // 請求隊列數量
    uint32_t notify_buf_size;    // 通知緩衝區大小 (忽略)
};
```

**狀態結構** (device.h:384-408):
```c
typedef struct {
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;

    uint32_t QueueSel;
    virtio_fs_queue_t queues[3];  // 隊列 0: 高優先級, 1+: 請求隊列

    uint32_t Status;
    uint32_t InterruptStatus;

    uint32_t *ram;

    char *mount_tag;              // 掛載標籤
    char *shared_dir;             // host 共享目錄路徑

    inode_map_entry *inode_map;   // inode → 路徑映射表

    void *priv;
} virtio_fs_state_t;
```

### 5.6.2 Inode 映射機制

VirtIO-FS 使用 inode 號碼作為文件標識符:

**映射結構** (device.h:369-373):
```c
typedef struct inode_map_entry {
    uint64_t ino;                    // inode 號碼
    char *path;                      // 完整路徑
    struct inode_map_entry *next;    // 鏈表下一節點
} inode_map_entry;
```

**查找函數** (virtio-fs.c:39-47):
```c
inode_map_entry *find_inode_path(inode_map_entry *head, uint64_t ino)
{
    while (head) {
        if (head->ino == ino)
            return head;
        head = head->next;
    }
    return NULL;
}
```

**特殊 inode**:
- **inode=1**: 根目錄 (映射到 `vfs->shared_dir`)
- **其他 inode**: 通過 `stat()` 系統調用獲取的實際 inode

### 5.6.3 FUSE 協議實現

**請求結構**:
```c
struct fuse_in_header {
    uint32_t len;       // 請求總長度
    uint32_t opcode;    // 操作碼 (FUSE_INIT, FUSE_GETATTR, ...)
    uint64_t unique;    // 唯一請求 ID
    uint64_t nodeid;    // 目標 inode
    uint32_t uid;       // 用戶 ID
    uint32_t gid;       // 組 ID
    uint32_t pid;       // 進程 ID
    uint32_t padding;
};

struct fuse_out_header {
    uint32_t len;       // 回應總長度
    int32_t error;      // 錯誤碼 (0 = 成功)
    uint64_t unique;    // 對應請求的 unique
};
```

**支持的操作** (virtio.h:27-46):
```c
#define FUSE_INIT        26  // 初始化
#define FUSE_GETATTR      3  // 獲取文件屬性
#define FUSE_OPENDIR     27  // 打開目錄
#define FUSE_READDIRPLUS 44  // 讀取目錄項 (含屬性)
#define FUSE_LOOKUP       1  // 查找文件名
#define FUSE_FORGET       2  // 忘記 inode
#define FUSE_RELEASEDIR  29  // 關閉目錄
#define FUSE_OPEN        14  // 打開文件
#define FUSE_READ        15  // 讀取文件
#define FUSE_RELEASE     18  // 關閉文件
#define FUSE_FLUSH       25  // 刷新緩衝
#define FUSE_DESTROY     38  // 銷毀文件系統
```

### 5.6.4 核心操作實現

#### 1. FUSE_INIT - 初始化協商

**實現** (virtio-fs.c:105-129):
```c
static void virtio_fs_init_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_init_out *init_out =
        (struct fuse_init_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    // 設置回應頭
    header_resp->out.len = sizeof(struct fuse_out_header) +
                           sizeof(struct fuse_init_out);
    header_resp->out.error = 0;

    // 協商特性和參數
    init_out->major = 7;
    init_out->minor = 41;
    init_out->max_readahead = 0x10000;
    init_out->flags = FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_DO_READDIRPLUS;
    init_out->max_background = 64;
    init_out->congestion_threshold = 32;
    init_out->max_write = 0x131072;  // ~1MB
    init_out->time_gran = 1;  // 1 納秒時間精度

    *plen = header_resp->out.len;
}
```

#### 2. FUSE_GETATTR - 獲取文件屬性

**實現** (virtio-fs.c:131-184):
```c
static void virtio_fs_getattr_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t inode = in_header->nodeid;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_attr_out *outattr =
        (struct fuse_attr_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    // 設置回應長度
    header_resp->out.len = sizeof(struct fuse_out_header) +
                           sizeof(struct fuse_attr_out);
    header_resp->out.error = 0;

    // 解析 inode 到路徑
    const char *target_path = NULL;
    struct stat st;

    if (inode == 1) {
        target_path = vfs->shared_dir;  // 根目錄
    } else {
        inode_map_entry *entry = find_inode_path(vfs->inode_map, inode);
        if (!entry) {
            header_resp->out.error = -ENOENT;  // 文件不存在
            *plen = sizeof(struct fuse_out_header);
            return;
        }
        target_path = entry->path;
    }

    // 獲取文件屬性
    if (stat(target_path, &st) < 0) {
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        return;
    }

    // 填充屬性
    outattr->attr_valid = 60;  // 屬性緩存 60 秒
    outattr->attr_valid_nsec = 0;
    outattr->attr.ino = st.st_ino;
    outattr->attr.size = st.st_size;
    outattr->attr.blocks = st.st_blocks;
    outattr->attr.atime = st.st_atime;
    outattr->attr.mtime = st.st_mtime;
    outattr->attr.ctime = st.st_ctime;
    outattr->attr.mode = st.st_mode;
    outattr->attr.nlink = st.st_nlink;
    outattr->attr.uid = st.st_uid;
    outattr->attr.gid = st.st_gid;
    outattr->attr.blksize = st.st_blksize;

    *plen = header_resp->out.len;
}
```

#### 3. FUSE_OPENDIR - 打開目錄

**實現** (virtio-fs.c:186-234):
```c
static void virtio_fs_opendir_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t nodeid = in_header->nodeid;

    // 查找 inode 對應的路徑
    inode_map_entry *entry = find_inode_path(vfs->inode_map, nodeid);
    if (!entry)
        return;

    // 打開目錄
    DIR *dir = opendir(entry->path);
    if (!dir)
        return;

    // 分配目錄句柄
    dir_handle_t *handle = malloc(sizeof(dir_handle_t));
    if (!handle) {
        closedir(dir);
        return;
    }
    handle->dir = dir;

    // 複製路徑字串
    size_t path_len = strlen(entry->path) + 1;
    handle->path = malloc(path_len);
    if (!handle->path) {
        closedir(dir);
        free(handle);
        return;
    }
    memcpy(handle->path, entry->path, path_len);

    // 返回句柄作為文件句柄
    struct fuse_open_out *open_out =
        (struct fuse_open_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);
    memset(open_out, 0, sizeof(*open_out));
    open_out->fh = (uint64_t) handle;  // 指針作為句柄
    open_out->open_flags = 0;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header) +
                           sizeof(struct fuse_open_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}
```

#### 4. FUSE_READDIRPLUS - 讀取目錄內容

這是最複雜的操作,返回目錄中所有項目及其屬性 (virtio-fs.c:236-300+):

```c
static void virtio_fs_readdirplus_handler(virtio_fs_state_t *vfs,
                                          struct virtq_desc vq_desc[4],
                                          uint32_t *plen)
{
    struct fuse_read_in *read_in =
        (struct fuse_read_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    dir_handle_t *handle = (dir_handle_t *) (uintptr_t) read_in->fh;
    if (!handle || !handle->dir)
        return;

    DIR *dir = handle->dir;
    const char *dir_path = handle->path;

    uintptr_t base = (uintptr_t) vfs->ram + vq_desc[3].addr;
    size_t offset = 0;

    // 重新掃描目錄
    rewinddir(dir);
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        // 跳過 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // 構建完整路徑
        size_t dir_len = strlen(dir_path);
        size_t name_len = strlen(entry->d_name);
        size_t full_len = dir_len + 1 + name_len + 1;  // dir + '/' + name + '\0'

        char *full_path = (char *) malloc(full_len);
        if (!full_path)
            continue;

        memcpy(full_path, dir_path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, entry->d_name, name_len);
        full_path[full_len - 1] = '\0';

        // 獲取文件屬性
        struct stat st;
        if (stat(full_path, &st) < 0) {
            free(full_path);
            continue;
        }

        // 寫入 fuse_entry_out (文件屬性)
        struct fuse_entry_out *entry_out = (struct fuse_entry_out *) (base + offset);
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->nodeid = st.st_ino;
        entry_out->attr.ino = st.st_ino;
        entry_out->attr.mode = st.st_mode;
        entry_out->attr.nlink = st.st_nlink;
        entry_out->attr.size = st.st_size;
        entry_out->attr.atime = st.st_atime;
        entry_out->attr.mtime = st.st_mtime;
        entry_out->attr.ctime = st.st_ctime;
        entry_out->attr.uid = st.st_uid;
        entry_out->attr.gid = st.st_gid;
        entry_out->attr.blksize = st.st_blksize;
        entry_out->attr.blocks = st.st_blocks;

        // 寫入 fuse_direntplus (目錄項信息)
        struct fuse_direntplus *direntplus =
            (struct fuse_direntplus *) (base + offset + sizeof(*entry_out));
        memset(direntplus, 0, sizeof(*direntplus));
        direntplus->dirent.ino = st.st_ino;
        direntplus->dirent.off = offset;
        direntplus->dirent.namelen = name_len;
        direntplus->dirent.type = (st.st_mode >> 12) & 0xF;  // 文件類型

        // 寫入文件名
        memcpy(direntplus->dirent.name, entry->d_name, name_len);

        // 計算對齊後的大小 (8 字節對齊)
        size_t entry_size = sizeof(*entry_out) +
                            sizeof(*direntplus) +
                            name_len;
        entry_size = (entry_size + 7) & ~7;  // 對齊到 8 字節

        offset += entry_size;
        free(full_path);
    }

    // 設置回應長度
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header) + offset;
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}
```

### 5.6.5 VirtIO-FS 數據流

```
完整的 READDIRPLUS 請求流程
┌──────────────────────────────────────────────────────────┐
│ Guest 驅動                                               │
│   ↓ 1. 寫入 QueueNotify                                  │
├──────────────────────────────────────────────────────────┤
│ semu VirtIO-FS 設備                                      │
│   ↓ 2. 讀取 Available Ring                               │
│   ↓ 3. 解析描述符鏈:                                     │
│      ┌────────────────────────────────────────┐          │
│      │ desc[0]: fuse_in_header (nodeid=...)  │          │
│      │ desc[1]: fuse_read_in (fh=handle)     │          │
│      │ desc[2]: fuse_out_header (回應頭)     │          │
│      │ desc[3]: 數據緩衝區 (目錄項列表)      │          │
│      └────────────────────────────────────────┘          │
│   ↓ 4. 調用 virtio_fs_readdirplus_handler()             │
├──────────────────────────────────────────────────────────┤
│ Host 文件系統                                            │
│   ↓ 5. opendir() / readdir() / stat()                   │
│   ↓ 6. 返回目錄項信息                                    │
├──────────────────────────────────────────────────────────┤
│ semu VirtIO-FS 設備                                      │
│   ↓ 7. 格式化 FUSE 回應到 guest RAM                      │
│   ↓ 8. 更新 Used Ring                                    │
│   ↓ 9. 觸發中斷 (VIRTIO_INT__USED_RING)                 │
├──────────────────────────────────────────────────────────┤
│ Guest 驅動                                               │
│   ↓ 10. 處理中斷,讀取 Used Ring                          │
│   ↓ 11. 解析目錄項數據                                   │
└──────────────────────────────────────────────────────────┘
```

---

## 5.7 其他 VirtIO 設備

### 5.7.1 VirtIO-Sound

提供音訊輸入/輸出功能 (device.h:316-361):

```c
typedef struct {
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;

    uint32_t QueueSel;
    virtio_snd_queue_t queues[4];  // 控制、事件、TX、RX 隊列

    uint32_t Status;
    uint32_t InterruptStatus;

    uint32_t *ram;
    void *priv;
} virtio_snd_state_t;
```

### 5.7.2 VirtIO-RNG (隨機數生成器)

提供硬體隨機數源,用於熵收集 (device.h 中定義,實現在 virtio-rng.c)。

---

## 5.8 性能優化技術

### 5.8.1 零拷貝技術

1. **磁盤 I/O**: 使用 `mmap()` 將磁盤文件直接映射到記憶體
   ```c
   uint32_t *disk_mem = mmap(NULL, disk_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, disk_fd, 0);
   ```

2. **網路 I/O**: 使用 `readv()`/`writev()` 直接在 guest RAM 上操作
   ```c
   plen = readv(tap->tap_fd, iovs_cursor, niovs);
   ```

3. **VirtIO 描述符**: 直接指向 guest RAM,無需緩衝
   ```c
   buffer_iovs[i].iov_base = (void *) ((uintptr_t) ram + desc_addr);
   ```

### 5.8.2 批量處理

VirtIO 設計支持批量提交請求:
```c
// 處理所有可用請求直到隊列為空
while (queue->last_avail != new_avail) {
    // 處理單個請求
    ...
    queue->last_avail++;
    new_used++;
}
// 一次性更新 Used Ring 和觸發中斷
```

### 5.8.3 非阻塞 I/O

所有外設 I/O 都使用非阻塞模式:
```c
// UART
struct pollfd pfd = {uart->in_fd, POLLIN, 0};
poll(&pfd, 1, 0);  // 超時 0 = 立即返回

// VirtIO Net
struct pollfd pfd = {tap->tap_fd, POLLIN | POLLOUT, 0};
poll(&pfd, 1, 0);
```

---

## 5.9 知識檢查點

完成本階段後,你應該能夠:

- [ ] 解釋 8250 UART 的暫存器佈局和中斷機制
- [ ] 理解 VirtIO MMIO 暫存器的統一接口
- [ ] 繪製 VirtIO 隊列的三部分結構 (描述符、可用環、已用環)
- [ ] 解釋 VirtIO Block 如何使用 3 個描述符表示一個請求
- [ ] 分析 VirtIO Network 的零拷貝 iovec 處理流程
- [ ] 理解 VirtIO-FS 的 inode 映射機制
- [ ] 識別週邊設備系統中的性能優化技術
- [ ] 解釋 UART coroutine 整合如何避免忙等待

---

## 5.10 實踐練習

1. **UART 調試**:
   - 在 `u8250_handle_in()` 中添加日誌,追蹤鍵盤輸入
   - 實驗退出序列 `Ctrl-a x` 的處理邏輯

2. **VirtIO Block 追蹤**:
   - 在 `virtio_blk_desc_handler()` 中打印扇區號和請求類型
   - 觀察 Linux 啟動時的磁盤讀取模式

3. **網路封包捕獲**:
   - 在 `handle_read()`/`handle_write()` 中記錄封包大小
   - 分析網路流量模式

4. **VirtIO-FS 文件訪問**:
   - 在 `virtio_fs_getattr_handler()` 中記錄訪問的路徑
   - 追蹤 `ls` 命令的 FUSE 操作序列

---

## 5.11 延伸閱讀

- **VirtIO 規範**: [OASIS VirtIO 1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)
- **8250 UART**: [16550 UART Datasheet](https://www.ti.com/lit/ds/symlink/pc16550d.pdf)
- **FUSE 協議**: [Linux FUSE Documentation](https://www.kernel.org/doc/html/latest/filesystems/fuse.html)
- **Zero-copy I/O**: [Efficient data transfer through zero copy](https://developer.ibm.com/articles/j-zerocopy/)

---

**下一階段**: [階段 6: 進階主題 - 網路後端與調試技術](stage6-advanced.md)
