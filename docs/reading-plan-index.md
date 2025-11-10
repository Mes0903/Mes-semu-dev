# SEMU 閱讀計畫索引

> 系統化學習 RISC-V 模擬器的完整路徑

---

## 📖 文檔導覽

### 核心文檔

- **[專案架構概覽](./project-overview.md)** - 整體架構與設計理念
- **[數據結構詳解](./data-structures.md)** - 核心數據結構定義
- **本文檔** - 完整閱讀計畫

### 階段性學習文檔

1. **[階段 1：建立整體概念](./stage1-overview.md)** ⭐⭐⭐
2. **[階段 2：CPU 核心機制](./stage2-cpu-core.md)** ⭐⭐⭐
3. **[階段 3：系統服務層](./stage3-system-services.md)** ⭐⭐⭐
4. **[階段 4：中斷系統](./stage4-interrupts.md)** ⭐⭐
5. **[階段 5：外設設備](./stage5-peripherals.md)** ⭐⭐
6. **[階段 6：進階主題](./stage6-advanced.md)** ⭐

---

## 🎯 學習路徑總覽

```
開始
  │
  ├─> 階段 1: 建立整體概念 (1-2 天)
  │   ├─ 理解 SEMU 設計理念
  │   ├─ 掌握系統啟動流程
  │   ├─ 熟悉記憶體映射
  │   └─ 理解數據結構關係
  │
  ├─> 階段 2: CPU 核心機制 (3-4 天)
  │   ├─ 指令解碼與執行
  │   ├─ MMU 虛擬記憶體
  │   ├─ TLB 快取實現
  │   └─ 異常處理機制
  │
  ├─> 階段 3: 系統服務層 (2-3 天)
  │   ├─ SBI 標準實現
  │   ├─ HSM 多核心管理
  │   ├─ IPI 與 RFENCE
  │   └─ 協程式調度
  │
  ├─> 階段 4: 中斷系統 (1-2 天)
  │   ├─ PLIC 中斷控制器
  │   ├─ ACLINT (MTIMER/MSWI/SSWI)
  │   └─ 中斷路由與處理
  │
  ├─> 階段 5: 外設設備 (3-4 天)
  │   ├─ UART 串口
  │   ├─ VirtIO 框架
  │   └─ 各種 VirtIO 設備
  │
  └─> 階段 6: 進階主題 (依興趣)
      ├─ 網路後端實現
      ├─ 協程式 SMP 深入
      ├─ 性能優化技巧
      └─ 除錯與測試
```

---

## 📋 階段 1：建立整體概念

**預估時間**：1-2 天
**優先級**：⭐⭐⭐（必讀）

### 學習目標

- ✅ 理解 SEMU 的設計理念（極簡、可讀、模組化）
- ✅ 掌握系統啟動流程（main → semu_init → hart_exec_loop）
- ✅ 熟悉記憶體映射佈局（RAM、MMIO、ACLINT）
- ✅ 理解頂層數據結構關係（emu_state_t、hart_t、vm_t）
- ✅ 了解設備樹的作用

### 閱讀清單

#### 文檔
- [README.md](../README.md)
- [專案架構概覽](./project-overview.md)
- [數據結構詳解](./data-structures.md)
- [階段 1 詳細文檔](./stage1-overview.md)

#### 代碼
- [main.c](../main.c):1-200 - `main()` 和 `semu_init()`
- [main.c](../main.c):600-800 - 記憶體映射 `semu_mem_load/store`
- [main.c](../main.c):1100-1200 - 主循環 `hart_exec_loop()`
- [device.h](../device.h):428-461 - `emu_state_t` 結構
- [riscv.h](../riscv.h):78-170 - `hart_t` 結構
- [minimal.dts](../minimal.dts) - 設備樹源碼

### 實踐練習

1. 編譯並運行 SEMU，觀察啟動過程
2. 修改記憶體大小，觀察變化
3. 使用 GDB 追蹤第一條指令
4. 理解記憶體映射路由機制

### 知識檢查點

- [ ] 能說出 SEMU 的三個核心設計理念
- [ ] 能畫出記憶體映射圖
- [ ] 能描述系統啟動的 6 個步驟
- [ ] 能解釋設備樹的作用
- [ ] 理解 `emu_state_t` 與 `hart_t` 的關係

---

## 📋 階段 2：CPU 核心機制

**預估時間**：3-4 天
**優先級**：⭐⭐⭐（必讀）

### 學習目標

- ✅ 掌握 RISC-V 指令解碼流程
- ✅ 理解 RV32IMA 指令集實現
- ✅ 深入理解 MMU 虛擬記憶體轉譯（Sv32）
- ✅ 掌握 8×2 組相聯 TLB 快取設計
- ✅ 理解異常處理與特權級切換

### 閱讀清單

#### 文檔
- [階段 2 詳細文檔](./stage2-cpu-core.md)
- [RISC-V Privileged Spec](https://riscv.org/technical/specifications/)

#### 代碼
- [riscv.c](../riscv.c):800-1074 - `vm_step()` 主執行循環
- [riscv.c](../riscv.c):200-400 - 指令解碼
- [riscv.c](../riscv.c):400-600 - 指令執行（ALU、Load/Store）
- [riscv.c](../riscv.c):50-120 - `mmu_translate()` 頁表遍歷
- [riscv.c](../riscv.c):120-200 - TLB 快取查詢與更新
- [riscv.c](../riscv.c):1-50 - `mmu_invalidate_range()` 快取失效
- [riscv.c](../riscv.c):600-700 - `hart_trap()` 異常處理
- [riscv.c](../riscv.c):500-600 - CSR 讀寫
- [riscv_private.h](../riscv_private.h) - 指令編碼、異常代碼

### 核心知識點

#### 指令執行流程
```
vm_step(hart)
  ↓
Fetch (指令抓取)
  ├─ 查詢 cache_fetch
  └─ MMU 轉譯
  ↓
Decode (解碼)
  ├─ 提取 opcode
  ├─ 提取 funct3/funct7
  └─ 解析立即數
  ↓
Execute (執行)
  ├─ ALU 運算
  ├─ Load/Store
  └─ 分支/跳轉
  ↓
Writeback (寫回)
  └─ 更新 x_regs
  ↓
Update PC
  ↓
Check Interrupts
```

#### MMU 轉譯流程
```
虛擬地址 (VA)
  ↓
提取 VPN 和 Offset
  ↓
查詢 TLB 快取
  ├─ 命中 → 返回 PPN
  └─ 未命中 ↓
     頁表遍歷 (Sv32 兩級)
       ↓
     更新 TLB (LRU 替換)
       ↓
     返回 PPN
  ↓
物理地址 = (PPN << 12) | offset
```

#### TLB 快取設計
```
cache_load[8]  // 8 組
  └─ mmu_cache_set_t
      ├─ ways[0]  // 第一路
      │   ├─ n_pages (VPN)
      │   └─ phys_ppn (PPN)
      ├─ ways[1]  // 第二路
      │   ├─ n_pages
      │   └─ phys_ppn
      └─ lru (0 或 1)

索引計算：
set_idx = __builtin_parity(vpn) & 0x7  // 3-bit parity hash
```

### 實踐練習

1. 追蹤一條 ADDI 指令的完整執行過程
2. 觀察 MMU 頁表遍歷（使用 GDB）
3. 觸發一個頁面錯誤，觀察異常處理
4. 測量 TLB 快取命中率

### 知識檢查點

- [ ] 能說出 RV32IMA 中各字母的含義
- [ ] 能畫出指令執行的 5 個階段
- [ ] 能解釋 Sv32 的兩級頁表結構
- [ ] 能說明 3-bit parity hash 的作用
- [ ] 理解 LRU 替換策略的實現
- [ ] 能描述異常處理的完整流程

---

## 📋 階段 3：系統服務層

**預估時間**：2-3 天
**優先級**：⭐⭐⭐（必讀）

### 學習目標

- ✅ 掌握 SBI 0.2 標準實現
- ✅ 理解 HSM 多核心管理機制
- ✅ 理解 IPI 與 RFENCE 實現
- ✅ 深入理解協程式 SMP 調度

### 閱讀清單

#### 文檔
- [階段 3 詳細文檔](./stage3-system-services.md)
- [SBI Specification 0.2](https://github.com/riscv-non-isa/riscv-sbi-doc)

#### 代碼
- [main.c](../main.c):400-800 - `handle_sbi_ecall()` SBI 實現
- [main.c](../main.c):250-350 - HSM 擴展實現
- [main.c](../main.c):350-400 - IPI 和 RFENCE 實現
- [coro.c](../coro.c):1-200 - `coro_init()` 和 `coro_sched()`
- [coro.c](../coro.c):200-400 - `coro_yield()` 和喚醒機制
- [coro.c](../coro.c):400-615 - 事件驅動等待

### 核心知識點

#### SBI 擴展
```
SBI_EID_BASE     (0x10)
  ├─ GET_SBI_SPEC_VERSION
  ├─ GET_SBI_IMPL_ID
  └─ PROBE_EXTENSION

SBI_EID_TIMER    (0x54494D45)
  └─ SET_TIMER → 設置 mtimecmp

SBI_EID_HSM      (0x48534D)
  ├─ HART_START → 啟動次級核心
  ├─ HART_STOP → 停止核心
  ├─ HART_GET_STATUS → 查詢狀態
  └─ HART_SUSPEND → 掛起核心

SBI_EID_IPI      (0x735049)
  └─ SEND_IPI → 設置 sip[SSI]

SBI_EID_RFENCE   (0x52464E43)
  ├─ RFENCE_I → 失效指令快取
  ├─ RFENCE_VMA → 失效 TLB
  └─ RFENCE_VMA_ASID → 失效特定 ASID
```

#### 協程調度流程
```
hart_exec_loop()
  ↓
coro_schedule()
  ├─ 選擇下一個可運行 hart
  ├─ 切換協程上下文 (swapcontext)
  └─ 返回到 vm_step()
  ↓
vm_step() 執行指令
  ↓
WFI 指令 → hart->wfi()
  ↓
coro_yield()
  ├─ 標記 hart 為 WAITING
  ├─ 切換到調度器協程
  └─ 等待事件喚醒
  ↓
事件發生 (計時器/UART/IPI)
  ↓
coro_wakeup(hart_id)
  └─ 標記 hart 為 RUNNABLE
```

### 實踐練習

1. 使用 SBI 啟動次級核心
2. 發送 IPI 給其他核心
3. 觀察協程切換過程
4. 測試 WFI 事件驅動機制

### 知識檢查點

- [ ] 能列出 6 個 SBI 擴展及其用途
- [ ] 能描述 HSM 的 7 種核心狀態
- [ ] 理解 IPI 的完整流程
- [ ] 能解釋 RFENCE 與 TLB 失效的關係
- [ ] 理解協程與執行緒的區別
- [ ] 能說明事件驅動的優勢

---

## 📋 階段 4：中斷系統

**預估時間**：1-2 天
**優先級**：⭐⭐（重要）

### 學習目標

- ✅ 理解 PLIC 中斷控制器工作原理
- ✅ 掌握 ACLINT 三個組件（MTIMER、MSWI、SSWI）
- ✅ 理解中斷路由與優先級
- ✅ 掌握中斷處理流程

### 閱讀清單

#### 文檔
- [階段 4 詳細文檔](./stage4-interrupts.md)
- [RISC-V ACLINT Spec](https://github.com/riscv/riscv-aclint)

#### 代碼
- [plic.c](../plic.c) - 完整閱讀（128 行）
- [aclint.c](../aclint.c):1-150 - MTIMER 和 MSWI
- [aclint.c](../aclint.c):150-227 - SSWI
- [main.c](../main.c):350-400 - IPI 實現

### 核心知識點

#### 中斷流程
```
外設觸發中斷
  ↓
plic.active |= (1 << IRQ_NUM)
  ↓
plic.ip |= (1 << IRQ_NUM)
  ↓
if (plic.ie[context] & bit)
  hart->sip |= RV_INT_SEI_BIT
  ↓
if (sip & sie & sstatus_sie)
  觸發中斷
  ↓
hart_trap()
  ├─ 保存 PC → sepc
  ├─ 保存特權模式 → sstatus_spp
  ├─ 設置 scause
  └─ 跳轉到 stvec_addr
```

#### ACLINT 組件
```
MTIMER (0x0D000000)
  ├─ mtime (64-bit) → 當前時間
  └─ mtimecmp[hartid] → 比較值
      if (mtime >= mtimecmp[i])
          hart[i]->sip |= STI_BIT

MSWI (0x0C000000)
  └─ msip[hartid]
      if (msip[i] == 1)
          hart[i]->sip |= SSI_BIT (M-mode)

SSWI (0x0E000000)
  └─ ssip[hartid]
      if (ssip[i] == 1)
          hart[i]->sip |= SSI_BIT (S-mode)
```

### 實踐練習

1. 觸發一個 UART 中斷，追蹤處理過程
2. 設置計時器中斷，觀察 Linux 調度
3. 使用 IPI 喚醒其他核心
4. 分析中斷延遲

### 知識檢查點

- [ ] 能畫出完整的中斷路由圖
- [ ] 能區分 SSI、STI、SEI 三種中斷
- [ ] 理解 PLIC 的 context 概念
- [ ] 能說明 MTIMER 如何實現定時中斷
- [ ] 理解 MSWI 與 SSWI 的區別

---

## 📋 階段 5：外設設備

**預估時間**：3-4 天
**優先級**：⭐⭐（重要）

### 學習目標

- ✅ 理解 UART 8250 實現
- ✅ 掌握 VirtIO 標準框架
- ✅ 理解 VirtQueue 環緩衝機制
- ✅ 學習各種 VirtIO 設備實現

### 閱讀清單

#### 文檔
- [階段 5 詳細文檔](./stage5-peripherals.md)
- [VirtIO Spec 1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/)

#### 代碼
- [uart.c](../uart.c) - 完整閱讀（259 行）
- [virtio.h](../virtio.h) - VirtIO 常數定義
- [virtio-blk.c](../virtio-blk.c):1-200 - VirtQueue 處理框架
- [virtio-net.c](../virtio-net.c) - 網路設備（選讀）
- [virtio-fs.c](../virtio-fs.c) - 文件系統（選讀）

### 核心知識點

#### UART 暫存器
```
0x0: RBR/THR (接收/發送緩衝)
0x1: IER (中斷使能)
0x2: IIR (中斷識別)
0x3: LCR (線路控制)
0x4: MCR (數據機控制)
0x5: LSR (線路狀態)
0x6: MSR (數據機狀態)
0x7: SCR (暫存器)
```

#### VirtIO 環緩衝
```
Guest Driver 提交請求：
  ↓
Descriptor Table
  ├─ [0] { addr, len, flags, next }
  ├─ [1] { addr, len, flags, next }
  └─ ...
  ↓
Available Ring
  ├─ idx (更新)
  └─ ring[idx] = descriptor_head
  ↓
Device 處理
  ├─ 從 last_avail 讀取
  ├─ 遍歷描述符鏈
  └─ 執行 I/O
  ↓
Used Ring
  ├─ idx (更新)
  └─ ring[idx] = { id, len }
  ↓
觸發中斷
```

### 實踐練習

1. 通過 UART 發送和接收字元
2. 使用 VirtIO-Blk 讀寫磁碟
3. 配置 VirtIO-Net 網路
4. 測試 VirtIO-FS 文件共享

### 知識檢查點

- [ ] 能說出 UART 8250 的關鍵暫存器
- [ ] 理解 VirtIO Feature 協商過程
- [ ] 能畫出 VirtQueue 的三個環結構
- [ ] 理解描述符鏈的用途
- [ ] 能描述 VirtIO-Net 的 TX/RX 流程

---

## 📋 階段 6：進階主題

**預估時間**：依興趣而定
**優先級**：⭐（選讀）

### 學習目標

- ✅ 深入理解網路後端實現（TAP、vmnet、SLIRP）
- ✅ 掌握協程式 SMP 的實現細節
- ✅ 學習性能優化技巧
- ✅ 了解除錯與測試方法

### 閱讀清單

#### 文檔
- [階段 6 詳細文檔](./stage6-advanced.md)
- [網路配置文檔](../docs/networking.md)

#### 代碼
- [netdev.c](../netdev.c) - 網路抽象層
- [netdev-vmnet.c](../netdev-vmnet.c) - macOS 後端
- [slirp.c](../slirp.c) - 用戶模式網路
- [coro.c](../coro.c) - 完整閱讀（深入理解）

### 核心知識點

#### 網路後端
```
TAP (Linux)
  ├─ 內核 TAP 設備
  ├─ 橋接模式
  └─ 需要 root 權限

vmnet (macOS)
  ├─ Apple 原生框架
  ├─ 共享模式
  └─ 無需 root

SLIRP (跨平台)
  ├─ 用戶模式網路堆疊
  ├─ NAT 模式
  └─ 無需特權
```

### 實踐練習

1. 配置 TAP 網路（Linux）
2. 使用 vmnet 共享網路（macOS）
3. 測試 SLIRP 模式
4. 測量 MMU 快取命中率

### 知識檢查點

- [ ] 能說明三種網路後端的優缺點
- [ ] 理解 ucontext 的實現原理
- [ ] 能使用 GDB 除錯模擬器
- [ ] 了解性能優化的關鍵點

---

## ⏱️ 時間規劃建議

### 快速瀏覽（2-3 天）

適合想快速了解整體架構的讀者。

```
Day 1: 階段 1 (建立整體概念)
  ├─ 上午：閱讀文檔
  └─ 下午：運行和觀察

Day 2: 階段 2 (CPU 核心) + 階段 3 (系統服務)
  ├─ 上午：指令執行
  └─ 下午：SBI 和協程

Day 3: 階段 4 (中斷) + 階段 5 (外設)
  ├─ 上午：中斷系統
  └─ 下午：VirtIO 框架
```

### 深入學習（1-2 週）

適合想深入理解每個模組的讀者。

```
Week 1:
  Day 1-2: 階段 1
  Day 3-5: 階段 2 (重點)
  Day 6-7: 階段 3 (重點)

Week 2:
  Day 1-2: 階段 4
  Day 3-5: 階段 5
  Day 6-7: 實踐練習 + 複習
```

### 完全掌握（3-4 週）

適合想成為專家的讀者。

```
Week 1: 階段 1 + 階段 2
Week 2: 階段 3 + 階段 4
Week 3: 階段 5
Week 4: 階段 6 + 專案實踐
```

---

## 📚 額外學習資源

### RISC-V 規範

- [RISC-V ISA Manual](https://riscv.org/technical/specifications/)
- [RISC-V Privileged Specification](https://riscv.org/technical/specifications/)
- [RISC-V Assembly Programmer's Manual](https://github.com/riscv-non-isa/riscv-asm-manual)

### 虛擬化與模擬

- [VirtIO Specification](https://docs.oasis-open.org/virtio/virtio/v1.1/)
- [QEMU Documentation](https://www.qemu.org/docs/master/)

### 系統程式設計

- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/)

---

## 🎯 學習檢查清單

### 基礎掌握

- [ ] 能解釋 SEMU 的設計理念
- [ ] 能畫出系統架構圖
- [ ] 理解記憶體映射佈局
- [ ] 能描述啟動流程
- [ ] 掌握核心數據結構

### 核心掌握

- [ ] 能說明指令執行的完整流程
- [ ] 理解 MMU 虛擬記憶體轉譯
- [ ] 掌握 TLB 快取實現
- [ ] 理解 SBI 各擴展的作用
- [ ] 掌握中斷處理流程

### 進階掌握

- [ ] 能實現自己的 VirtIO 設備
- [ ] 理解協程調度的實現細節
- [ ] 能優化 MMU 快取性能
- [ ] 能除錯複雜的系統問題
- [ ] 能修改和擴展 SEMU

---

## 💡 學習建議

### 閱讀技巧

1. **先看整體後看細節**
   - 第一遍快速瀏覽，建立整體概念
   - 第二遍細讀重點部分
   - 第三遍實踐並驗證

2. **結合代碼閱讀**
   - 不要只看文檔
   - 對照源碼理解實現
   - 使用 IDE 的跳轉功能

3. **繪製圖表**
   - 畫出數據結構關係圖
   - 畫出流程圖
   - 有助於理解和記憶

4. **實踐驗證**
   - 每學完一個模組就動手實踐
   - 修改代碼觀察變化
   - 使用 GDB 除錯

### 常見問題

**Q: 我沒有 RISC-V 基礎，能學習嗎？**
A: 可以！SEMU 代碼註釋豐富，配合 RISC-V 規範文檔學習即可。

**Q: 需要多少時間？**
A: 快速瀏覽 2-3 天，深入學習 1-2 週，完全掌握 3-4 週。

**Q: 哪些部分最重要？**
A: 階段 1、2、3 是核心，必須掌握。階段 4、5 重要但可選讀。階段 6 依興趣而定。

**Q: 遇到不懂的怎麼辦？**
A: 查閱 RISC-V 規範、使用 GDB 除錯、閱讀相關源碼註釋。

---

## 📖 相關文檔

- [專案架構概覽](./project-overview.md)
- [數據結構詳解](./data-structures.md)
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
