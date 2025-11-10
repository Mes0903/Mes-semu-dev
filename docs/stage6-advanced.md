# 階段 6:進階主題 (Stage 6: Advanced Topics)

本文檔探討 semu 的高級特性,包括網路後端實現、性能優化技術和調試方法。

## 6.1 概述 (Overview)

進階主題涵蓋以下領域:

```
進階主題架構
├── 網路後端 (Network Backends)
│   ├── TAP (Linux)
│   ├── vmnet (macOS)
│   └── slirp (User mode)
│
├── 性能優化 (Performance Optimizations)
│   ├── 零拷貝 I/O
│   ├── 記憶體映射
│   ├── 批量處理
│   └── 非阻塞 I/O
│
├── 調試技術 (Debugging Techniques)
│   ├── GDB 調試
│   ├── 追蹤工具
│   └── 性能分析
│
└── 擴展與定制 (Extensions)
    ├── 新設備添加
    ├── 自定義指令
    └── 系統調用追蹤
```

---

## 6.2 網路後端實現 (Network Backend Implementations)

semu 支持三種網路後端,每種都有不同的用例和性能特徵。

### 6.2.1 架構總覽

**netdev 抽象層** (netdev.h):
```c
typedef enum {
    NETDEV_IMPL_tap,    // Linux TUN/TAP
    NETDEV_IMPL_user,   // Slirp 用戶模式網路
    NETDEV_IMPL_vmnet,  // macOS vmnet.framework
} netdev_impl_t;

typedef struct {
    netdev_impl_t type;  // 後端類型
    void *op;            // 後端專屬狀態
} netdev_t;
```

**初始化流程** (netdev.c:78-158):
```c
bool netdev_init(netdev_t *netdev, const char *net_type)
{
#if defined(__APPLE__)
    /* macOS: vmnet 或 user */
    if (!net_type || strcmp(net_type, "vmnet") == 0) {
        netdev->type = NETDEV_IMPL_vmnet;
        netdev->op = calloc(1, sizeof(net_vmnet_options_t));

        if (net_vmnet_init(netdev, SEMU_VMNET_SHARED, NULL) != 0) {
            // vmnet 失敗,回退到 user 模式
            fprintf(stderr, "vmnet init failed, falling back to user mode...\n");
            free(netdev->op);
            netdev->op = NULL;
        } else {
            return true;  // vmnet 成功
        }
    }

    // 初始化 user 模式 (slirp)
    if (!net_type || strcmp(net_type, "user") == 0) {
        netdev->type = NETDEV_IMPL_user;
        if (!netdev->op) {
            netdev->op = malloc(sizeof(net_user_options_t));
        }
        return net_init_user(netdev) == 0;
    }
#else
    /* Linux: tap 或 user */
    int dev_idx = find_net_dev_idx(net_type, netdev_impl_lookup);
    if (dev_idx == -1)
        return false;

    netdev->type = dev_idx;
    switch (dev_idx) {
    case NETDEV_IMPL_tap:
        netdev->op = malloc(sizeof(net_tap_options_t));
        net_init_tap(netdev);
        break;
    case NETDEV_IMPL_user:
        netdev->op = malloc(sizeof(net_user_options_t));
        net_init_user(netdev);
        break;
    }
#endif

    return true;
}
```

---

### 6.2.2 TAP 後端 (Linux)

TAP 是 Linux 上的標準虛擬網路接口,提供內核級別的網路性能。

**初始化流程** (netdev.c:43-64):
```c
static int net_init_tap(netdev_t *netdev)
{
    net_tap_options_t *tap = (net_tap_options_t *) netdev->op;

    // 1. 打開 TUN/TAP 設備
    tap->tap_fd = open("/dev/net/tun", O_RDWR);
    if (tap->tap_fd < 0) {
        fprintf(stderr, "failed to open TAP device: %s\n", strerror(errno));
        return false;
    }

    // 2. 配置接口參數
    struct ifreq ifreq = {
        .ifr_flags = IFF_TAP | IFF_NO_PI  // TAP 模式,無協議信息
    };
    strncpy(ifreq.ifr_name, "tap%d", sizeof(ifreq.ifr_name));

    // 3. 創建 TAP 接口
    if (ioctl(tap->tap_fd, TUNSETIFF, &ifreq) < 0) {
        fprintf(stderr, "failed to allocate TAP device: %s\n", strerror(errno));
        return false;
    }

    fprintf(stderr, "allocated TAP interface: %s\n", ifreq.ifr_name);

    // 4. 設置非阻塞模式
    int flags = fcntl(tap->tap_fd, F_GETFL, 0);
    fcntl(tap->tap_fd, F_SETFL, flags | O_NONBLOCK);

    return 0;
}
```

**TAP 網路配置** (手動操作):
```bash
# 1. 啟動 TAP 接口
sudo ip link set tap0 up

# 2. 分配 IP 地址
sudo ip addr add 10.0.2.1/24 dev tap0

# 3. 啟用 NAT 轉發 (允許 guest 訪問外網)
sudo iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
sudo iptables -A FORWARD -i tap0 -o eth0 -j ACCEPT
sudo iptables -A FORWARD -i eth0 -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT

# 4. 啟用 IP 轉發
sudo sysctl -w net.ipv4.ip_forward=1
```

**TAP I/O 操作** (virtio-net.c:153-166):
```c
case NETDEV_IMPL_tap: {
    net_tap_options_t *tap = (net_tap_options_t *) netdev->op;

    // 讀取封包:直接零拷貝讀取到 guest RAM
    plen = readv(tap->tap_fd, iovs_cursor, niovs);

    // 寫入封包:直接零拷貝寫入
    plen = writev(tap->tap_fd, iovs_cursor, niovs);

    // 檢查非阻塞狀態
    if (plen < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        queue->fd_ready = false;
        return -1;
    }
    break;
}
```

**TAP 優勢**:
- ✅ **最高性能**: 內核級別處理,零拷貝
- ✅ **完整功能**: 支持所有網路協議
- ✅ **低延遲**: 直接與內核網路棧交互
- ❌ **需要權限**: 需要 root 或 CAP_NET_ADMIN
- ❌ **手動配置**: 需要設置網路橋接/NAT

---

### 6.2.3 vmnet 後端 (macOS)

vmnet.framework 是 Apple 提供的虛擬網路框架,支持三種模式。

#### vmnet 架構

**狀態結構** (netdev.h):
```c
typedef struct {
    interface_ref iface;        // vmnet 接口引用
    dispatch_queue_t queue;     // GCD 隊列
    dispatch_semaphore_t sem;   // 同步信號量

    pthread_mutex_t lock;       // 線程鎖
    int pipe_fds[2];            // pipe 用於 poll() 整合

    uint8_t mac[6];             // MAC 地址
    bool running;               // 運行狀態
} net_vmnet_state_t;
```

#### vmnet 三種模式

**1. Shared Mode (NAT + DHCP)** - netdev-vmnet.c:58-140

默認模式,提供自動 NAT 和 DHCP 配置:

```c
static int vmnet_init_shared(net_vmnet_state_t *state)
{
    // 1. 創建 vmnet 配置
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
                              VMNET_SHARED_MODE);

    // 2. 創建 GCD 隊列和信號量
    state->sem = dispatch_semaphore_create(0);
    state->queue = dispatch_queue_create("org.semu.vmnet.shared",
                                         DISPATCH_QUEUE_SERIAL);

    // 3. 啟動 vmnet 接口
    __block interface_ref iface = NULL;
    __block vmnet_return_t status = VMNET_FAILURE;

    iface = vmnet_start_interface(
        iface_desc, state->queue,
        ^(vmnet_return_t ret, xpc_object_t param) {
            status = ret;
            if (ret == VMNET_SUCCESS) {
                // 提取 MAC 地址
                const char *mac_str = xpc_dictionary_get_string(
                    param, vmnet_mac_address_key);
                if (mac_str) {
                    sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                           &state->mac[0], &state->mac[1], &state->mac[2],
                           &state->mac[3], &state->mac[4], &state->mac[5]);
                }

                // 設置封包接收回調
                vmnet_interface_set_event_callback(
                    iface, VMNET_INTERFACE_PACKETS_AVAILABLE, state->queue,
                    ^(interface_event_t event_id, xpc_object_t event) {
                        // 批量讀取封包 (最多 32 個)
                        struct vmpktdesc pkts[32];
                        uint8_t bufs[32][VMNET_BUF_SIZE];
                        struct iovec iovs[32];
                        int pkt_cnt = 32;

                        for (int i = 0; i < pkt_cnt; i++) {
                            iovs[i].iov_base = bufs[i];
                            iovs[i].iov_len = VMNET_BUF_SIZE;
                            pkts[i].vm_pkt_size = VMNET_BUF_SIZE;
                            pkts[i].vm_pkt_iov = &iovs[i];
                            pkts[i].vm_pkt_iovcnt = 1;
                            pkts[i].vm_flags = 0;
                        }

                        int received = pkt_cnt;
                        vmnet_return_t ret = vmnet_read(iface, pkts, &received);

                        // 處理每個封包
                        for (int i = 0; i < received; i++) {
                            vmnet_packet_handler(state, bufs[i],
                                                pkts[i].vm_pkt_size);
                        }
                    });
            }
            dispatch_semaphore_signal(state->sem);
        });

    // 4. 等待初始化完成
    dispatch_semaphore_wait(state->sem, DISPATCH_TIME_FOREVER);

    if (status != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] failed to create interface: %d\n", status);
        return -1;
    }

    state->iface = iface;
    return 0;
}
```

**vmnet Shared Mode 自動配置**:
- Guest IP: 192.168.64.x (自動 DHCP 分配)
- Gateway: 192.168.64.1
- DNS: 192.168.64.1
- NAT: 自動啟用

**2. Host Mode (隔離網路)**

VM 之間可以通信,但無法訪問外網:

```c
static int vmnet_init_host(net_vmnet_state_t *state)
{
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
                              VMNET_HOST_MODE);

    // ... 與 shared mode 類似的初始化 ...

    return 0;
}
```

**3. Bridged Mode (橋接到物理網路)**

直接橋接到物理網卡,guest 獲得與 host 同網段的 IP:

```c
static int vmnet_init_bridged(net_vmnet_state_t *state,
                              const char *iface_name)
{
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
                              VMNET_BRIDGED_MODE);

    // 指定橋接的物理接口 (例如 en0)
    if (iface_name && strlen(iface_name) > 0) {
        xpc_dictionary_set_string(iface_desc,
                                  vmnet_shared_interface_name_key,
                                  iface_name);
    }

    // ... 初始化 ...

    return 0;
}
```

#### vmnet 事件處理

**封包接收流程** (netdev-vmnet.c:30-56):
```c
static void vmnet_packet_handler(net_vmnet_state_t *state,
                                 uint8_t *buf,
                                 ssize_t len)
{
    if (len <= 0)
        return;

    pthread_mutex_lock(&state->lock);

    // 1. 寫入封包長度 (4 字節)
    uint32_t pkt_len = (uint32_t) len;
    write(state->pipe_fds[1], &pkt_len, sizeof(pkt_len));

    // 2. 寫入封包數據
    write(state->pipe_fds[1], buf, len);

    pthread_mutex_unlock(&state->lock);
}
```

**為什麼使用 pipe?**

vmnet 使用 GCD (Grand Central Dispatch) 進行異步事件處理,而 semu 的主循環使用 `poll()` 進行同步 I/O。pipe 作為橋梁:

```
vmnet 異步事件流
┌──────────────────────────────────────────────────┐
│ 網路封包到達                                     │
│   ↓                                              │
│ vmnet_read() (在 GCD 隊列中)                     │
│   ↓                                              │
│ vmnet_packet_handler()                           │
│   ↓                                              │
│ write(pipe_fds[1], pkt, len)  ← 寫入 pipe       │
├──────────────────────────────────────────────────┤
│ semu 主循環 (poll)                               │
│   ↓                                              │
│ poll(pipe_fds[0], POLLIN)  ← 檢測 pipe 可讀     │
│   ↓                                              │
│ net_vmnet_read()  ← 從 pipe 讀取                 │
│   ↓                                              │
│ virtio_net_try_rx()  ← 傳遞給 guest              │
└──────────────────────────────────────────────────┘
```

**讀取封包** (netdev-vmnet.c:350-398):
```c
ssize_t net_vmnet_read(net_vmnet_state_t *state, uint8_t *buf, size_t len)
{
    // 1. 讀取封包長度
    uint32_t pkt_len;
    ssize_t n = read(state->pipe_fds[0], &pkt_len, sizeof(pkt_len));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;  // 沒有數據
        return -1;
    }

    if (n != sizeof(pkt_len)) {
        fprintf(stderr, "[vmnet] partial read of packet size\n");
        return -1;
    }

    // 2. 檢查緩衝區大小
    if (pkt_len > len) {
        // 排空過大的封包
        uint8_t tmp[VMNET_BUF_SIZE];
        size_t remaining = pkt_len;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
            read(state->pipe_fds[0], tmp, chunk);
            remaining -= chunk;
        }
        return -1;
    }

    // 3. 讀取封包數據
    ssize_t total = 0;
    while (total < pkt_len) {
        n = read(state->pipe_fds[0], buf + total, pkt_len - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        total += n;
    }

    return total;
}
```

**寫入封包 (零拷貝)** (netdev-vmnet.c:428-456):
```c
ssize_t net_vmnet_writev(net_vmnet_state_t *state,
                         const struct iovec *iov,
                         size_t iovcnt)
{
    if (!state->running || !state->iface)
        return -1;

    // 計算總長度
    size_t total_len = 0;
    for (size_t i = 0; i < iovcnt; i++)
        total_len += iov[i].iov_len;

    // 準備 vmpktdesc (直接使用 iovec,零拷貝)
    struct vmpktdesc pkt;
    pkt.vm_pkt_size = total_len;
    pkt.vm_pkt_iov = (struct iovec *) iov;  // 直接傳遞 iovec
    pkt.vm_pkt_iovcnt = iovcnt;
    pkt.vm_flags = 0;

    // 寫入封包
    int pkt_cnt = 1;
    vmnet_return_t ret = vmnet_write(state->iface, &pkt, &pkt_cnt);

    if (ret != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] writev failed: %d\n", ret);
        return -1;
    }

    return pkt_cnt > 0 ? total_len : -1;
}
```

**vmnet 優勢**:
- ✅ **易用性**: 自動 NAT/DHCP (shared mode)
- ✅ **零拷貝**: 支持 writev 直接傳遞 iovec
- ✅ **批量接收**: 一次最多讀取 32 個封包
- ❌ **需要權限**: 需要 root 或 entitlement
- ❌ **macOS 專屬**: 不可移植

---

### 6.2.4 Slirp 用戶模式網路

Slirp 是用戶空間 TCP/IP 協議棧,無需任何權限即可使用。

#### Slirp 架構

**組件**:
```
Slirp 架構
┌─────────────────────────────────────────────┐
│ semu (VirtIO Net)                           │
│   ↓ 寫入封包                                │
├─────────────────────────────────────────────┤
│ guest_to_host_channel (pipe)                │
│   ↓                                         │
├─────────────────────────────────────────────┤
│ libslirp (用戶空間協議棧)                   │
│   ├── TCP 狀態機                            │
│   ├── UDP 處理                              │
│   ├── DHCP 服務器 (10.0.2.2)               │
│   ├── DNS 代理 (10.0.2.3)                   │
│   └── NAT 轉換                              │
│   ↓ 處理後的封包                            │
├─────────────────────────────────────────────┤
│ host_to_guest_channel (pipe)                │
│   ↓                                         │
├─────────────────────────────────────────────┤
│ semu → 傳遞給 guest                         │
└─────────────────────────────────────────────┘
```

**狀態結構** (netdev.h):
```c
typedef struct {
    Slirp *slirp;               // libslirp 實例
    slirp_timer *timer;         // Slirp 定時器

    // 雙向 pipe 通道
    int guest_to_host_channel[2];  // guest → slirp
    int host_to_guest_channel[2];  // slirp → guest

    // poll() 支持
    struct pollfd *pfd;
    int pfd_len;
    int pfd_size;

    virtio_net_state_t *peer;   // VirtIO Net 設備
} net_user_options_t;
```

#### Slirp 初始化

**創建 Slirp 實例** (slirp.c:175-217):
```c
Slirp *slirp_create(net_user_options_t *usr, SlirpConfig *cfg)
{
    // 配置 Slirp 網路
    cfg->version = SLIRP_CHECK_VERSION(4, 8, 0) ? 6
                   : SLIRP_CHECK_VERSION(4, 7, 0) ? 4
                   : 1;
    cfg->restricted = 0;       // 允許所有連接
    cfg->in_enabled = 1;       // IPv4 啟用

    // IPv4 地址配置
    inet_pton(AF_INET, "10.0.2.0", &(cfg->vnetwork));
    inet_pton(AF_INET, "255.255.255.0", &(cfg->vnetmask));
    inet_pton(AF_INET, "10.0.2.2", &(cfg->vhost));  // Gateway
    inet_pton(AF_INET, "10.0.2.15", &(cfg->vdhcp_start));  // Guest IP
    inet_pton(AF_INET, "10.0.2.3", &(cfg->vnameserver));  // DNS

    // IPv6 配置
    cfg->in6_enabled = 1;
    inet_pton(AF_INET6, "fd00::", &cfg->vprefix_addr6);
    inet_pton(AF_INET6, "fd00::3", &cfg->vnameserver6);

    // 其他參數
    cfg->vhostname = "slirp";
    cfg->if_mtu = 1500;
    cfg->if_mru = 1500;

    // 創建 Slirp 實例
    Slirp *slirp = slirp_new(cfg, &slirp_cb, usr);

    return slirp;
}
```

**Slirp 網路配置**:
- Guest IP: 10.0.2.15
- Gateway: 10.0.2.2
- DNS: 10.0.2.3
- Network: 10.0.2.0/24

#### Slirp 回調函數

Slirp 通過回調與 semu 交互 (slirp.c:102-113):

```c
static const SlirpCb slirp_cb = {
    .send_packet = net_slirp_send_packet,         // Slirp → guest
    .guest_error = net_slirp_guest_error,         // 錯誤報告
    .clock_get_ns = net_slirp_clock_get_ns,       // 獲取時間
    .init_completed = net_slirp_init_completed,   // 初始化完成
    .timer_new_opaque = net_slirp_timer_new_opaque,  // 創建定時器
    .timer_free = net_slirp_timer_free,           // 釋放定時器
    .timer_mod = net_slirp_timer_mod,             // 修改定時器
    .register_poll_socket = net_slirp_register_poll_sock,
    .unregister_poll_socket = net_slirp_unregister_poll_sock,
    .notify = net_slirp_notify,
};
```

**發送封包回調** (slirp.c:11-16):
```c
static ssize_t net_slirp_send_packet(const void *buf, size_t len, void *opaque)
{
    net_user_options_t *usr = (net_user_options_t *) opaque;

    // 寫入 pipe,傳遞給 guest
    return write(usr->guest_to_host_channel[SLIRP_WRITE_SIDE], buf, len);
}
```

#### Slirp 數據流

**從 guest 接收** (slirp.c:159-173):
```c
int net_slirp_read(net_user_options_t *usr)
{
    uint8_t pkt[1514];

    // 從 pipe 讀取 guest 發送的封包
    ssize_t plen = read(usr->host_to_guest_channel[SLIRP_READ_SIDE],
                        pkt, sizeof(pkt));
    if (plen < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;  // 沒有數據
        return -1;
    }

    // 傳遞給 Slirp 處理
    slirp_input(usr->slirp, pkt, plen);

    return plen;
}
```

**完整數據流**:
```
Slirp 封包流程
┌────────────────────────────────────────────────┐
│ 1. Guest 發送 TCP SYN 到 93.184.216.34:80     │
│    (example.com)                               │
│    ↓                                           │
│ 2. virtio_net_try_tx()                         │
│    ↓                                           │
│ 3. handle_write() → write(host_to_guest_ch)   │
│    ↓                                           │
├────────────────────────────────────────────────┤
│ 4. poll() 檢測到 pipe 可讀                     │
│    ↓                                           │
│ 5. net_slirp_read()                            │
│    ↓                                           │
│ 6. slirp_input() ← 進入 Slirp 處理             │
│    ├── 解析 TCP SYN                            │
│    ├── 創建 socket(AF_INET, SOCK_STREAM)      │
│    ├── connect(93.184.216.34, 80)             │
│    ├── 發送 TCP SYN/ACK 給 guest               │
│    └── 調用 send_packet 回調                   │
│    ↓                                           │
│ 7. net_slirp_send_packet()                     │
│    ↓                                           │
│ 8. write(guest_to_host_ch)  ← 寫入 pipe       │
├────────────────────────────────────────────────┤
│ 9. poll() 檢測到 pipe 可讀                     │
│    ↓                                           │
│ 10. virtio_net_try_rx()                        │
│    ↓                                           │
│ 11. handle_read() → readv(guest_to_host_ch)   │
│    ↓                                           │
│ 12. Guest 收到 TCP SYN/ACK                     │
└────────────────────────────────────────────────┘
```

#### Slirp 定時器

Slirp 使用定時器處理 TCP 超時、重傳等:

**定時器回調** (slirp.c:45-49):
```c
static void net_slirp_timer_cb(void *opaque)
{
    slirp_timer *t = opaque;
    slirp_handle_timer(t->slirp, t->id, t->cb_opaque);
}
```

**修改定時器** (slirp.c:76-82):
```c
static void net_slirp_timer_mod(void *timer,
                                int64_t expire_time,
                                void *opaque)
{
    slirp_timer *t = (slirp_timer *) timer;
    semu_timer_rebase(&t->timer, expire_time);
}
```

**Slirp 優勢**:
- ✅ **無需權限**: 完全用戶空間運行
- ✅ **可移植**: 跨平台 (Linux, macOS, Windows)
- ✅ **易用**: 自動配置 NAT/DHCP/DNS
- ❌ **性能較低**: 用戶空間協議棧開銷
- ❌ **功能受限**: 不支持 ICMP (ping)、原始套接字

---

### 6.2.5 網路後端比較

| 特性 | TAP (Linux) | vmnet (macOS) | Slirp (User) |
|------|-------------|---------------|--------------|
| **性能** | ⭐⭐⭐⭐⭐ (最快) | ⭐⭐⭐⭐ (快) | ⭐⭐⭐ (中等) |
| **權限** | 需要 root | 需要 root/entitlement | 無需權限 |
| **配置** | 手動配置 | 自動配置 | 自動配置 |
| **平台** | Linux only | macOS only | 跨平台 |
| **協議** | 全支持 | 全支持 | TCP/UDP only |
| **NAT** | 手動 iptables | 自動 | 自動 |
| **DHCP** | 需外部 DHCP | 自動 | 自動 |

---

## 6.3 性能優化技術 (Performance Optimization Techniques)

### 6.3.1 零拷貝 I/O (Zero-Copy I/O)

**概念**:

傳統 I/O 需要多次複製數據:
```
傳統 I/O
┌─────────────────┐
│ 1. 磁盤         │
│    ↓ DMA        │
│ 2. 內核緩衝區   │
│    ↓ copy_to_user
│ 3. 用戶緩衝區   │
│    ↓ memcpy     │
│ 4. 目標緩衝區   │
└─────────────────┘
4 次數據複製,2 次上下文切換
```

零拷貝避免不必要的複製:
```
零拷貝 I/O
┌─────────────────┐
│ 1. 磁盤         │
│    ↓ DMA        │
│ 2. 共享記憶體   │ ← 直接訪問
│    (mmap/iovec) │
└─────────────────┘
0-1 次數據複製
```

**semu 中的零拷貝實現**:

1. **磁盤 I/O (mmap)**:
```c
// virtio-blk.c:468-470
uint32_t *disk_mem = mmap(NULL, disk_size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, disk_fd, 0);

// 讀寫直接在映射記憶體上操作
memcpy(dest, src, len);  // 實際上是修改 mmap 區域,自動同步到磁盤
```

2. **網路 I/O (iovec)**:
```c
// virtio-net.c:155
plen = readv(tap->tap_fd, iovs_cursor, niovs);

// iovec 直接指向 guest RAM
buffer_iovs[i].iov_base = (void *) ((uintptr_t) ram + desc_addr);
buffer_iovs[i].iov_len = desc_len;

// 零拷貝寫入
plen = writev(tap->tap_fd, iovs_cursor, niovs);
```

3. **VirtIO 描述符**:
```c
// 描述符直接引用 guest RAM 地址
desc->addr = 0x80001000;  // guest RAM 中的偏移

// 設備直接讀寫 guest RAM
uint8_t *data = (uint8_t *) ((uintptr_t) ram + desc->addr);
```

**零拷貝優勢**:
- 減少 CPU 使用率 (50-80%)
- 降低記憶體頻寬需求
- 提高 I/O 吞吐量

### 6.3.2 批量處理 (Batching)

**VirtIO 批量請求處理** (virtio-blk.c:220-251):
```c
// 處理所有可用請求
while (queue->last_avail != new_avail) {
    uint16_t queue_idx = queue->last_avail % queue->QueueNum;
    uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                          (16 * (queue_idx % 2));

    // 處理單個請求
    virtio_blk_desc_handler(vblk, queue, buffer_idx, &len);

    // 寫入已用隊列
    ram[vq_used_addr] = buffer_idx;
    ram[vq_used_addr + 1] = len;

    queue->last_avail++;
    new_used++;
}

// 一次性更新已用隊列索引
vblk->ram[queue->QueueUsed] &= MASK(16);
vblk->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16;

// 一次性發送中斷
if (!(ram[queue->QueueAvail] & 1))
    vblk->InterruptStatus |= VIRTIO_INT__USED_RING;
```

**批量優勢**:
- 減少中斷次數 (降低開銷)
- 提高緩存效率
- 攤平固定開銷

**vmnet 批量接收** (netdev-vmnet.c:98-122):
```c
// 一次讀取最多 32 個封包
struct vmpktdesc pkts[32];
uint8_t bufs[32][VMNET_BUF_SIZE];
int pkt_cnt = 32;

int received = pkt_cnt;
vmnet_return_t ret = vmnet_read(iface, pkts, &received);

// 批量處理
for (int i = 0; i < received; i++) {
    vmnet_packet_handler(state, bufs[i], pkts[i].vm_pkt_size);
}
```

### 6.3.3 非阻塞 I/O與 poll()

**非阻塞 I/O 設置**:
```c
// uart.c:61-62
int flags = fcntl(tap->tap_fd, F_GETFL, 0);
fcntl(tap->tap_fd, F_SETFL, flags | O_NONBLOCK);
```

**poll() 多路復用** (virtio-net.c:387-397):
```c
// 檢查多個 fd 的就緒狀態
struct pollfd pfd = {tap->tap_fd, POLLIN | POLLOUT, 0};
poll(&pfd, 1, 0);  // 超時 0 = 立即返回

if (pfd.revents & POLLIN) {
    vnet->queues[VNET_QUEUE_RX].fd_ready = true;
    virtio_net_try_rx(vnet);
}
if (pfd.revents & POLLOUT) {
    vnet->queues[VNET_QUEUE_TX].fd_ready = true;
    virtio_net_try_tx(vnet);
}
```

### 6.3.4 記憶體對齊

**數據結構對齊** (device.h:26):
```c
#define PACKED(...)        \
    __pragma(pack(push, 1)) \
        __VA_ARGS__         \
        __pragma(pack(pop))
```

確保結構體與 VirtIO 規範對齊:
```c
PACKED(struct virtq_desc {
    uint64_t addr;    // 8 字節對齊
    uint32_t len;     // 4 字節對齊
    uint16_t flags;   // 2 字節對齊
    uint16_t next;
});
```

---

## 6.4 調試技術 (Debugging Techniques)

### 6.4.1 GDB 調試

**啟動 GDB**:
```bash
gdb --args build/semu -k linux-image
```

**常用 GDB 命令**:
```gdb
# 設置斷點
break riscv.c:978     # vm_step 函數
break uart.c:116      # UART 輸入

# 條件斷點
break virtio-blk.c:182 if type == VIRTIO_BLK_T_OUT

# 觀察點
watch vm->pc
watch uart->in_ready

# 追蹤
info registers
x/32xw 0x80000000     # 檢視 RAM
x/i vm->pc            # 反彙編當前指令

# Backtrace
bt
frame 3
info locals
```

**調試 CPU 執行**:
```gdb
# 單步執行指令
break vm_step
commands
  silent
  printf "PC: 0x%08x  Insn: 0x%08x\n", vm->pc, *(uint32_t*)(vm->ram + vm->pc - RAM_BASE)
  continue
end
```

### 6.4.2 追蹤工具

**添加追蹤宏**:
```c
#define TRACE_INSN(vm, insn) \
    fprintf(stderr, "[%d] PC=0x%08x INSN=0x%08x\n", vm->hart_id, vm->pc, insn)

#define TRACE_VIRTIO(dev, reg, val) \
    fprintf(stderr, "[VIRTIO-%s] %s = 0x%08x\n", #dev, #reg, val)
```

**使用示例**:
```c
// riscv.c
uint32_t insn = mem_ifetch(vm);
TRACE_INSN(vm, insn);

// virtio-blk.c
TRACE_VIRTIO(BLK, QueueNotify, value);
```

**strace 追蹤系統調用**:
```bash
strace -e trace=open,read,write,ioctl ./build/semu -k linux-image 2>&1 | tee semu.trace
```

### 6.4.3 性能分析

**使用 perf**:
```bash
# 記錄性能數據
perf record -g ./build/semu -k linux-image

# 分析報告
perf report

# 熱點函數
perf top -p $(pidof semu)
```

**使用 valgrind**:
```bash
# 記憶體洩漏檢測
valgrind --leak-check=full ./build/semu -k linux-image

# Cachegrind (緩存分析)
valgrind --tool=cachegrind ./build/semu -k linux-image
```

**自定義性能計數器**:
```c
#include <time.h>

struct timespec start, end;
clock_gettime(CLOCK_MONOTONIC, &start);

// ... 要測量的代碼 ...

clock_gettime(CLOCK_MONOTONIC, &end);
uint64_t ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
              (end.tv_nsec - start.tv_nsec);
fprintf(stderr, "Elapsed: %lu ns\n", ns);
```

---

## 6.5 擴展與定制 (Extensions and Customization)

### 6.5.1 添加新設備

**步驟**:

1. **定義設備狀態** (device.h):
```c
#define IRQ_MYDEV 7
#define IRQ_MYDEV_BIT (1 << IRQ_MYDEV)

typedef struct {
    uint32_t Status;
    uint32_t InterruptStatus;
    uint32_t *ram;
    void *priv;
} mydev_state_t;
```

2. **實現 MMIO 讀寫** (mydev.c):
```c
void mydev_read(hart_t *vm, mydev_state_t *dev,
                uint32_t addr, uint8_t width, uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        // 讀取暫存器
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    }
}

void mydev_write(hart_t *vm, mydev_state_t *dev,
                 uint32_t addr, uint8_t width, uint32_t value)
{
    // 寫入暫存器
}
```

3. **在 riscv.c 中集成**:
```c
// mem_load/mem_store 中添加地址檢查
if (addr >= MYDEV_BASE && addr < MYDEV_BASE + MYDEV_SIZE) {
    mydev_read(vm, &vm->mydev, addr - MYDEV_BASE, width, value);
    return;
}
```

4. **在 device tree 中添加節點** (main.c):
```c
sprintf(dtb + dtb_len,
        "mydev@%lx {"
        "  compatible = \"semu,mydev\";"
        "  reg = <0x%lx 0x1000>;"
        "  interrupts = <%d>;"
        "};",
        MYDEV_BASE, MYDEV_BASE, IRQ_MYDEV);
```

### 6.5.2 自定義指令

**實現自定義指令** (riscv.c):
```c
// 在 vm_step() 中添加新的解碼邏輯
case 0x7b:  // 自定義操作碼
    {
        uint32_t funct3 = EXTRACT(insn, 14, 12);
        uint32_t funct7 = EXTRACT(insn, 31, 25);

        if (funct3 == 0x0 && funct7 == 0x01) {
            // 自定義指令: MY_INSN rd, rs1, rs2
            uint32_t rd = EXTRACT(insn, 11, 7);
            uint32_t rs1 = EXTRACT(insn, 19, 15);
            uint32_t rs2 = EXTRACT(insn, 24, 20);

            // 執行自定義操作
            vm->x_regs[rd] = my_custom_operation(vm->x_regs[rs1],
                                                  vm->x_regs[rs2]);
        } else {
            vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, insn);
        }
    }
    break;
```

### 6.5.3 系統調用追蹤

**攔截 ecall** (riscv.c):
```c
case 0x73:  // SYSTEM
    if (imm == 0x0) {  // ecall
        // 記錄系統調用
        fprintf(stderr, "[SYSCALL] a7=%lu a0=%lu a1=%lu\n",
                vm->x_regs[rv_reg_a7],
                vm->x_regs[rv_reg_a0],
                vm->x_regs[rv_reg_a1]);

        // 正常處理
        vm_set_exception(vm, RV_EXC_ECALL_FROM_U_OR_VU_MODE + vm->priv, 0);
    }
    break;
```

---

## 6.6 知識檢查點

完成本階段後,你應該能夠:

- [ ] 解釋三種網路後端的工作原理和適用場景
- [ ] 理解 vmnet 如何使用 pipe 橋接異步事件
- [ ] 分析 Slirp 的封包處理流程
- [ ] 識別 semu 中的零拷貝技術
- [ ] 使用 GDB 調試 RISC-V 指令執行
- [ ] 應用 perf/valgrind 進行性能分析
- [ ] 實現簡單的自定義設備
- [ ] 追蹤和分析系統調用

---

## 6.7 實踐練習

1. **網路性能測試**:
   - 使用 iperf3 比較三種網路後端的吞吐量
   - 分析 vmnet 和 TAP 的延遲差異

2. **添加調試輸出**:
   - 在 `virtio_blk_desc_handler` 中記錄所有磁盤操作
   - 統計每種操作類型的頻率

3. **自定義設備實現**:
   - 實現一個簡單的 GPIO 設備
   - 添加 LED 控制暫存器 (0: 關閉, 1: 開啟)

4. **性能優化**:
   - 測量 `vm_step` 函數的平均執行時間
   - 識別性能瓶頸並嘗試優化

5. **系統調用追蹤器**:
   - 實現完整的系統調用追蹤器
   - 記錄系統調用號、參數和返回值
   - 生成統計報告

---

## 6.8 延伸閱讀

- **Linux Networking**: [Understanding Linux Network Internals](https://www.oreilly.com/library/view/understanding-linux-network/0596002556/)
- **macOS vmnet**: [vmnet.framework Documentation](https://developer.apple.com/documentation/vmnet)
- **libslirp**: [Slirp GitHub Repository](https://gitlab.freedesktop.org/slirp/libslirp)
- **Zero-Copy I/O**: [Efficient data transfer through zero copy](https://developer.ibm.com/articles/j-zerocopy/)
- **Performance Analysis**: [Systems Performance by Brendan Gregg](https://www.brendangregg.com/systems-performance-2nd-edition-book.html)

---

## 6.9 總結

恭喜你完成了 semu 的完整學習旅程!你已經掌握了:

1. **系統架構** - 從啟動到設備管理的完整流程
2. **CPU 核心** - RISC-V 指令集和 MMU 實現
3. **系統服務** - SBI、HSM、IPI 和 coroutine 調度
4. **中斷系統** - PLIC 和 ACLINT 的完整設計
5. **週邊設備** - UART 和 VirtIO 設備框架
6. **進階技術** - 網路後端、性能優化和調試方法

你現在有能力:
- ✅ 閱讀和理解 semu 的任何部分
- ✅ 添加新的設備或功能
- ✅ 調試複雜的虛擬化問題
- ✅ 優化性能並分析瓶頸
- ✅ 擴展 semu 以支持更多 RISC-V 特性

**下一步建議**:
1. 深入研究 Linux kernel 的 RISC-V 移植
2. 實現更多 VirtIO 設備 (GPU, Input)
3. 添加 GDB stub 支持遠程調試
4. 優化 MMU 性能 (TLB 預取、大頁支持)
5. 實現 RV64 (64 位 RISC-V) 支持

---

**返回**: [閱讀計畫索引](reading-plan-index.md)
