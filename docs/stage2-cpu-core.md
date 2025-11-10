# 階段 2：CPU 核心機制

> 深入理解 RISC-V 指令執行、MMU 虛擬記憶體與 TLB 快取（預估 3-4 天）

---

## 📋 本階段目標

- ✅ 掌握 RISC-V 指令解碼與執行流程
- ✅ 理解 RV32IMA 指令集的完整實現
- ✅ 深入理解 Sv32 虛擬記憶體轉譯機制
- ✅ 掌握 8×2 組相聯 TLB 快取設計（核心優化）
- ✅ 理解異常處理與特權級切換
- ✅ 掌握 CSR 暫存器的讀寫機制

---

## 📚 閱讀清單

### 必讀文檔

- [RISC-V Unprivileged ISA Spec](https://riscv.org/technical/specifications/) - 指令集規範
- [RISC-V Privileged Spec](https://riscv.org/technical/specifications/) - 特權架構規範
- [數據結構詳解](./data-structures.md) - 回顧 hart_t 結構

### 必讀代碼

**核心執行循環**：
- [riscv.c](../riscv.c):978-1074 - `vm_step()` 主執行循環

**指令解碼**：
- [riscv.c](../riscv.c):62-181 - 指令解碼函數
- [riscv_private.h](../riscv_private.h):3-17 - 指令編碼常數

**MMU 與 TLB**：
- [riscv.c](../riscv.c):185-200 - `mmu_invalidate()` 完全失效
- [riscv.c](../riscv.c):202-251 - `mmu_invalidate_range()` 範圍失效
- [riscv.c](../riscv.c):256-270 - `mmu_set()` 設置頁表
- [riscv.c](../riscv.c):298-316 - `mmu_lookup()` 頁表遍歷
- [riscv.c](../riscv.c):318-355 - `mmu_translate()` 虛擬地址轉譯
- [riscv.c](../riscv.c):362-386 - `mmu_fetch()` 指令抓取
- [riscv.c](../riscv.c):388-447 - `mmu_load()` 載入操作（帶 TLB）
- [riscv.c](../riscv.c):449-517 - `mmu_store()` 儲存操作（帶 TLB）

**指令執行**：
- [riscv.c](../riscv.c):800-837 - `op_mul()` 乘法/除法指令
- [riscv.c](../riscv.c):839-864 - `op_rv32i()` 基本整數運算
- [riscv.c](../riscv.c):866-884 - `op_jmp()` 條件分支
- [riscv.c](../riscv.c):914-971 - `op_amo()` 原子操作

**異常處理**：
- [riscv.c](../riscv.c):519-585 - `hart_trap()` 陷阱處理
- [riscv.c](../riscv.c):587-689 - CSR 讀寫函數

---

## 🎯 核心概念

### 1. RISC-V 指令格式

RISC-V 有 6 種指令格式，每種格式決定了如何編碼立即數和暫存器。

```
31                                                         0
┌─────────────────────────────────────────────────────────┐
│                    R-type (暫存器運算)                    │
├───────┬─────┬─────┬────┬─────┬───────┤
│funct7 │ rs2 │ rs1 │f3 │ rd  │opcode │  ADD, SUB, SLL, XOR...
└───────┴─────┴─────┴────┴─────┴───────┘
  31-25  24-20 19-15 14-12 11-7   6-0

┌─────────────────────────────────────────────────────────┐
│                    I-type (立即數運算)                    │
├─────────────────┬─────┬────┬─────┬───────┤
│   imm[11:0]     │ rs1 │f3 │ rd  │opcode │  ADDI, LW, JALR...
└─────────────────┴─────┴────┴─────┴───────┘
      31-20        19-15 14-12 11-7  6-0

┌─────────────────────────────────────────────────────────┐
│                    S-type (儲存指令)                      │
├───────────┬─────┬─────┬────┬───────────┬───────┤
│imm[11:5]  │ rs2 │ rs1 │f3 │imm[4:0]  │opcode │  SW, SH, SB
└───────────┴─────┴─────┴────┴───────────┴───────┘
   31-25    24-20 19-15 14-12  11-7      6-0

┌─────────────────────────────────────────────────────────┐
│                    B-type (條件分支)                      │
├─┬─────────┬─────┬─────┬────┬───────┬─┬───────┤
│i│imm[10:5]│ rs2 │ rs1 │f3 │imm[4:1]│i│opcode │  BEQ, BNE...
│m│         │     │     │   │        │m│       │
│[│         │     │     │   │        │[│       │
│1│         │     │     │   │        │1│       │
│2│         │     │     │   │        │1│       │
│]│         │     │     │   │        │]│       │
└─┴─────────┴─────┴─────┴────┴───────┴─┴───────┘
31  30-25   24-20 19-15 14-12 11-8   7   6-0

┌─────────────────────────────────────────────────────────┐
│                    U-type (大立即數)                      │
├─────────────────────────────────┬─────┬───────┤
│         imm[31:12]              │ rd  │opcode │  LUI, AUIPC
└─────────────────────────────────┴─────┴───────┘
            31-12                  11-7   6-0

┌─────────────────────────────────────────────────────────┐
│                    J-type (無條件跳轉)                    │
├─┬─────────┬─┬─────────┬─────┬───────┤
│i│imm[19:12]│i│imm[10:1]│ rd  │opcode │  JAL
│m│         │m│         │     │       │
│[│         │[│         │     │       │
│2│         │1│         │     │       │
│0│         │1│         │     │       │
│]│         │]│         │     │       │
└─┴─────────┴─┴─────────┴─────┴───────┘
31  30-21   20  19-12   11-7   6-0
```

### 2. RV32IMA 指令集詳解

**RV32I**（基本整數指令集）：
```
整數運算：
  - ADD, SUB, AND, OR, XOR
  - SLL, SRL, SRA (左移、邏輯右移、算術右移)
  - SLT, SLTU (比較指令)

立即數運算：
  - ADDI, ANDI, ORI, XORI
  - SLLI, SRLI, SRAI
  - SLTI, SLTIU

載入/儲存：
  - LW, LH, LB (載入 word/halfword/byte)
  - LHU, LBU (無符號載入)
  - SW, SH, SB (儲存)

分支：
  - BEQ, BNE (相等/不等分支)
  - BLT, BGE (有符號比較)
  - BLTU, BGEU (無符號比較)

跳轉：
  - JAL (跳轉並連結)
  - JALR (間接跳轉並連結)

其他：
  - LUI (載入高位立即數)
  - AUIPC (相對 PC 加立即數)
  - ECALL, EBREAK (系統調用/斷點)
  - FENCE (記憶體柵欄)
```

**RV32M**（乘法擴展）：
```
乘法：
  - MUL (32×32 → 低32位)
  - MULH (有符號 32×32 → 高32位)
  - MULHSU (有符號×無符號 → 高32位)
  - MULHU (無符號 32×32 → 高32位)

除法：
  - DIV (有符號除法)
  - DIVU (無符號除法)
  - REM (有符號取餘)
  - REMU (無符號取餘)
```

**RV32A**（原子操作擴展）：
```
原子操作：
  - LR.W (載入並保留)
  - SC.W (條件儲存)
  - AMOSWAP.W (原子交換)
  - AMOADD.W (原子加法)
  - AMOXOR.W, AMOAND.W, AMOOR.W
  - AMOMIN.W, AMOMAX.W (有符號)
  - AMOMINU.W, AMOMAXU.W (無符號)
```

---

## 🔍 指令解碼詳解

### 1. 解碼遮罩定義

**位置**：[riscv.c](../riscv.c):66-90

```c
enum {
    // 基本欄位遮罩
    FR_RD        = 0b00000000000000000000111110000000,  // rd: bit 7-11
    FR_FUNCT3    = 0b00000000000000000111000000000000,  // funct3: bit 12-14
    FR_RS1       = 0b00000000000011111000000000000000,  // rs1: bit 15-19
    FR_RS2       = 0b00000001111100000000000000000000,  // rs2: bit 20-24

    // I-type 立即數
    FI_IMM_11_0  = 0b11111111111100000000000000000000,  // imm[11:0]: bit 20-31

    // S-type 立即數（分兩段）
    FS_IMM_4_0   = 0b00000000000000000000111110000000,  // imm[4:0]: bit 7-11
    FS_IMM_11_5  = 0b11111110000000000000000000000000,  // imm[11:5]: bit 25-31

    // B-type 立即數（重新排列）
    FB_IMM_11    = 0b00000000000000000000000010000000,  // imm[11]: bit 7
    FB_IMM_4_1   = 0b00000000000000000000111100000000,  // imm[4:1]: bit 8-11
    FB_IMM_10_5  = 0b01111110000000000000000000000000,  // imm[10:5]: bit 25-30
    FB_IMM_12    = 0b10000000000000000000000000000000,  // imm[12]: bit 31

    // U-type 立即數
    FU_IMM_31_12 = 0b11111111111111111111000000000000,  // imm[31:12]: bit 12-31

    // J-type 立即數（重新排列）
    FJ_IMM_19_12 = 0b00000000000011111111000000000000,  // imm[19:12]: bit 12-19
    FJ_IMM_11    = 0b00000000000100000000000000000000,  // imm[11]: bit 20
    FJ_IMM_10_1  = 0b01111111111000000000000000000000,  // imm[10:1]: bit 21-30
    FJ_IMM_20    = 0b10000000000000000000000000000000,  // imm[20]: bit 31
};
```

### 2. 立即數解碼函數

#### I-type（最簡單）

**位置**：[riscv.c](../riscv.c):101-104

```c
static inline uint32_t decode_i(uint32_t insn)
{
    // 提取 bit 20-31，符號擴展到 32 位
    return ((int32_t) (insn & FI_IMM_11_0)) >> 20;
}
```

**工作原理**：
```
原始指令：  XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
           ^^^^^^^^^^^^                    (bit 20-31)

遮罩後：    XXXXXXXXXXXX00000000000000000000

右移 20：   000000000000XXXXXXXXXXXX (bit 0-11)

符號擴展：  SSSSSSSSSSSSSSSSSSSSXXXXXXXXXXXX
           (S = bit 11 的值)
```

**範例**：`ADDI x5, x10, -100`
```
指令編碼：  1111 1001 1100 0101 0000 0010 1001 0011
           ^^^^^^^^^^^^                     (imm = -100 = 0xF9C)
                        ^^^^^ ^^^^^ ^^^^^    (rs1=10, rd=5, opcode)

decode_i() 提取：
  insn & FI_IMM_11_0 = 0xF9C00000
  右移 20 位          = 0x00000F9C
  符號擴展 (int32_t) = 0xFFFFFF9C (-100)
```

#### J-type（最複雜，需要重新排列）

**位置**：[riscv.c](../riscv.c):106-115

```c
static inline uint32_t decode_j(uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FJ_IMM_20);      // bit 20 → bit 20
    dst |= (insn & FJ_IMM_19_12) << 11;  // bit 19-12 → bit 30-23
    dst |= (insn & FJ_IMM_11) << 2;      // bit 11 → bit 13
    dst |= (insn & FJ_IMM_10_1) >> 9;    // bit 10-1 → bit 21-12
    return ((int32_t) dst) >> 11;   // 符號擴展並右移
}
```

**為何這麼複雜？**

J-type 指令的立即數在指令中的排列順序是亂的（為了硬體優化）：

```
指令格式：  [imm[20]|imm[10:1]|imm[11]|imm[19:12]|rd|opcode]
            bit 31  30-21      20      19-12      11-7 6-0

實際順序：  imm[20:1] (20 位，左移 1 位變成 21 位偏移)
```

**重組過程**：
```
Step 1: 提取 bit 31 (imm[20])
  insn:  X___________________________  (bit 31)
  結果:  X___________________________ (保留在 bit 31)

Step 2: 提取 bit 12-19 (imm[19:12]) → 左移 11 位到 bit 23-30
  insn:  ____________XXXXXXXX________
  結果:  _______XXXXXXXX_____________ (bit 23-30)

Step 3: 提取 bit 20 (imm[11]) → 左移 2 位到 bit 22
  insn:  ____________X_______________
  結果:  __________________X_________ (bit 22)

Step 4: 提取 bit 21-30 (imm[10:1]) → 右移 9 位到 bit 12-21
  insn:  _XXXXXXXXXX_________________
  結果:  ____________XXXXXXXXXX______ (bit 12-21)

最終組合：  [imm[20]|imm[19:12]|imm[11]|imm[10:1]|0]
           bit 31   30-23      22      21-12    11-0
```

#### B-type（類似 J-type）

**位置**：[riscv.c](../riscv.c):118-127

```c
static inline uint32_t decode_b(uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FB_IMM_12);           // bit 31 → bit 31
    dst |= (insn & FB_IMM_11) << 23;     // bit 7 → bit 30
    dst |= (insn & FB_IMM_10_5) >> 1;    // bit 25-30 → bit 24-29
    dst |= (insn & FB_IMM_4_1) << 12;    // bit 8-11 → bit 20-23
    return ((int32_t) dst) >> 19;   // 右移 19 位（因為最低位是 0）
}
```

#### S-type（兩段式）

**位置**：[riscv.c](../riscv.c):130-136

```c
static inline uint32_t decode_s(uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FS_IMM_11_5);    // 高 7 位
    dst |= (insn & FS_IMM_4_0) << 13;  // 低 5 位左移 13 位
    return ((int32_t) dst) >> 20;
}
```

### 3. 暫存器欄位解碼

**位置**：[riscv.c](../riscv.c):144-181

```c
/* 提取 rd（目標暫存器，bit 7-11） */
static inline uint8_t decode_rd(uint32_t insn)
{
    return (insn & FR_RD) >> 7;
}

/* 提取 rs1（源暫存器 1，bit 15-19） */
static inline uint8_t decode_rs1(uint32_t insn)
{
    return (insn & FR_RS1) >> 15;
}

/* 提取 rs2（源暫存器 2，bit 20-24） */
static inline uint8_t decode_rs2(uint32_t insn)
{
    return (insn & FR_RS2) >> 20;
}

/* 提取 funct3（功能碼，bit 12-14） */
static inline uint8_t decode_func3(uint32_t insn)
{
    return (insn & FR_FUNCT3) >> 12;
}

/* 提取 funct5（原子操作用，bit 27-31） */
static inline uint8_t decode_func5(uint32_t insn)
{
    return insn >> 27;
}

/* 讀取 rs1 的值 */
static inline uint32_t read_rs1(const hart_t *vm, uint32_t insn)
{
    return vm->x_regs[decode_rs1(insn)];
}

/* 讀取 rs2 的值 */
static inline uint32_t read_rs2(const hart_t *vm, uint32_t insn)
{
    return vm->x_regs[decode_rs2(insn)];
}
```

---

## 🧮 MMU 虛擬記憶體轉譯

### 1. Sv32 分頁機制概述

RISC-V Sv32 使用**兩級頁表**進行虛擬地址轉譯。

#### 虛擬地址結構

```
31                 22 21                 12 11            0
┌────────────────────┬────────────────────┬──────────────┐
│    VPN[1]          │    VPN[0]          │    Offset    │
│  (10 bits)         │  (10 bits)         │  (12 bits)   │
└────────────────────┴────────────────────┴──────────────┘
  Level 1 頁表索引     Level 0 頁表索引     頁內偏移

VPN (Virtual Page Number) = 虛擬頁號
Offset = 頁內偏移（4KB = 2^12 bytes）
```

#### 頁表項（PTE）結構

```
31                10 9  8  7  6  5  4  3  2  1  0
┌──────────────────┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
│       PPN        │RSW│D │A │G │U │X │W │R │V │
│  (22 bits)       │(2)│  │  │  │  │  │  │  │  │
└──────────────────┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘

PPN (Physical Page Number) = 物理頁號
V (Valid) = 有效位
R (Read) = 可讀
W (Write) = 可寫
X (eXecute) = 可執行
U (User) = 用戶模式可訪問
G (Global) = 全局映射
A (Accessed) = 已訪問
D (Dirty) = 已修改
RSW (Reserved for Software) = 軟體保留
```

#### PTE 類型判斷

```c
bits[3:0] (X W R V)   PTE 類型
─────────────────────────────────
  0 0 0 0             無效（頁面錯誤）
  0 0 0 1             指向下一級頁表
  0 0 1 0             保留（非法）
  0 0 1 1             只讀頁面
  0 1 0 0             保留（非法）
  0 1 0 1             保留（非法）
  0 1 1 0             保留（非法）
  0 1 1 1             可讀寫頁面
  1 0 0 0             保留（非法）
  1 0 0 1             可執行頁面
  1 0 1 0             保留（非法）
  1 0 1 1             可讀可執行頁面
  1 1 0 0             保留（非法）
  1 1 0 1             保留（非法）
  1 1 1 0             保留（非法）
  1 1 1 1             可讀寫執行頁面
```

### 2. 頁表遍歷實現

#### satp 暫存器結構

```
31     30                              0
┌──────┬──────────────────────────────┐
│ MODE │           PPN                │
│(1bit)│         (22 bits)            │
└──────┴──────────────────────────────┘

MODE = 0: 禁用分頁（物理地址模式）
MODE = 1: 啟用 Sv32 分頁
PPN = 根頁表的物理頁號
```

#### mmu_set() - 設置頁表

**位置**：[riscv.c](../riscv.c):256-270

```c
static void mmu_set(hart_t *vm, uint32_t satp)
{
    mmu_invalidate(vm);  // 清空所有快取

    if (satp >> 31) {  // MODE = 1（啟用分頁）
        // 提取根頁表 PPN（bit 0-21）
        uint32_t root_ppn = satp & MASK(22);

        // 驗證根頁表是否可訪問
        uint32_t *page_table = vm->mem_page_table(vm, root_ppn);
        if (!page_table)
            return;  // 無效頁表，不更新

        vm->page_table = page_table;  // 儲存主機指針
        satp &= ~(MASK(9) << 22);  // 清除保留位
    } else {  // MODE = 0（物理地址模式）
        vm->page_table = NULL;
        satp = 0;
    }
    vm->satp = satp;
}
```

#### mmu_lookup() - 頁表遍歷

**位置**：[riscv.c](../riscv.c):298-316

```c
static bool mmu_lookup(const hart_t *vm,
                       uint32_t vpn,       // 虛擬頁號
                       uint32_t **pte,     // 返回 PTE 指針
                       uint32_t *ppn)      // 返回物理頁號
{
    // ═══════════════════════════════════════════
    // Level 1 頁表遍歷
    // ═══════════════════════════════════════════

    // 使用 VPN[1]（高 10 位）作為索引
    uint32_t vpn1 = vpn >> 10;
    *pte = &(vm->page_table)[vpn1];

    // 讀取 PTE
    uint32_t pte_val = **pte;
    uint32_t pte_flags = pte_val & MASK(4);  // bit 0-3

    // 判斷 PTE 類型
    switch (pte_flags) {
    case 0b0001:  // 指向下一級頁表
        break;

    case 0b0011:  // 只讀葉節點（大頁，4MB）
    case 0b0111:  // 可讀寫葉節點
    case 0b1001:  // 可執行葉節點
    case 0b1011:  // 可讀可執行葉節點
    case 0b1111:  // 可讀寫執行葉節點
        *ppn = pte_val >> 10;  // 提取 PPN

        // 檢查大頁是否對齊（PPN[0] 必須為 0）
        if (unlikely((*ppn) & MASK(10))) {
            *pte = NULL;  // 未對齊，錯誤
            return true;
        }

        // 大頁：合併 VPN[0] 到 PPN
        *ppn |= vpn & MASK(10);
        return true;

    default:  // 其他組合都是無效的
        *pte = NULL;
        return true;
    }

    // ═══════════════════════════════════════════
    // Level 0 頁表遍歷
    // ═══════════════════════════════════════════

    // 取得 Level 0 頁表的物理地址
    uint32_t l0_ppn = pte_val >> 10;
    uint32_t *l0_page_table = vm->mem_page_table(vm, l0_ppn);
    if (!l0_page_table)
        return false;  // 無法訪問，返回錯誤

    // 使用 VPN[0]（低 10 位）作為索引
    uint32_t vpn0 = vpn & MASK(10);
    *pte = &l0_page_table[vpn0];

    // 讀取 PTE
    pte_val = **pte;
    pte_flags = pte_val & MASK(4);

    switch (pte_flags) {
    case 0b0001:  // Level 0 不應該再有指針
        *pte = NULL;
        return true;

    case 0b0011:  // 葉節點（4KB 頁面）
    case 0b0111:
    case 0b1001:
    case 0b1011:
    case 0b1111:
        *ppn = pte_val >> 10;
        return true;

    default:
        *pte = NULL;
        return true;
    }
}
```

**頁表遍歷流程圖**：

```
開始
  │
  ├─ 提取 VPN = VA >> 12
  │
  ├─ Level 1 索引 = VPN[1] = VPN >> 10
  │
  ├─ PTE1 = page_table[VPN[1]]
  │
  ├─ if (PTE1.V == 0)
  │     → 頁面錯誤
  │
  ├─ if (PTE1.R || PTE1.W || PTE1.X)
  │     → 大頁（4MB）
  │     → PPN = PTE1.PPN | VPN[0]
  │     → 返回
  │
  ├─ Level 0 頁表 = mem[PTE1.PPN]
  │
  ├─ PTE0 = Level0[VPN[0]]
  │
  ├─ if (PTE0.V == 0)
  │     → 頁面錯誤
  │
  ├─ if (PTE0.R || PTE0.W || PTE0.X)
  │     → 普通頁（4KB）
  │     → PPN = PTE0.PPN
  │     → 返回
  │
  └─ 其他情況 → 保留/非法
```

### 3. 地址轉譯與權限檢查

#### mmu_translate() - 完整轉譯

**位置**：[riscv.c](../riscv.c):318-355

```c
static void mmu_translate(hart_t *vm,
                          uint32_t *addr,           // 輸入虛擬地址，輸出物理地址
                          const uint32_t access_bits,  // 需要的權限位
                          const uint32_t set_bits,     // 要設置的位（A/D）
                          const bool skip_privilege_test,  // 跳過特權檢查
                          const uint8_t fault,         // 訪問錯誤代碼
                          const uint8_t pfault)        // 頁面錯誤代碼
{
    // 保存虛擬地址（用於異常報告）
    vm->exc_val = *addr;

    // 如果未啟用分頁，直接返回（物理地址模式）
    if (!vm->page_table)
        return;

    // ═══════════════════════════════════════════
    // 步驟 1：頁表遍歷
    // ═══════════════════════════════════════════

    uint32_t vpn = (*addr) >> RV_PAGE_SHIFT;
    uint32_t *pte_ref;
    uint32_t ppn = 0;

    bool ok = mmu_lookup(vm, vpn, &pte_ref, &ppn);
    if (unlikely(!ok)) {
        // 頁表訪問失敗（記憶體錯誤）
        vm_set_exception(vm, fault, *addr);
        return;
    }

    // ═══════════════════════════════════════════
    // 步驟 2：驗證 PTE 有效性
    // ═══════════════════════════════════════════

    if (!pte_ref) {
        // PTE 無效（V=0 或保留組合）
        vm_set_exception(vm, pfault, *addr);
        return;
    }

    uint32_t pte = *pte_ref;

    // 檢查 PPN 是否有效（bit 22-31 必須為 0）
    if (ppn >> 20) {
        vm_set_exception(vm, pfault, *addr);
        return;
    }

    // ═══════════════════════════════════════════
    // 步驟 3：權限檢查
    // ═══════════════════════════════════════════

    // 檢查訪問權限（R/W/X）
    if (!(pte & access_bits)) {
        vm_set_exception(vm, pfault, *addr);
        return;
    }

    // 檢查特權級別
    // pte[4] = U 位
    //   U=0: 只有 S-mode 可訪問
    //   U=1: U-mode 和 S-mode 都可訪問（除非 SUM=0）
    bool pte_is_user = pte & (1 << 4);

    if (!skip_privilege_test) {
        if (pte_is_user != !vm->s_mode) {
            // 特權不匹配
            vm_set_exception(vm, pfault, *addr);
            return;
        }
    }

    // ═══════════════════════════════════════════
    // 步驟 4：更新 A/D 位
    // ═══════════════════════════════════════════

    uint32_t new_pte = pte | set_bits;
    if (new_pte != pte) {
        *pte_ref = new_pte;  // 寫回 PTE（設置 A/D 位）
    }

    // ═══════════════════════════════════════════
    // 步驟 5：計算物理地址
    // ═══════════════════════════════════════════

    uint32_t offset = (*addr) & MASK(RV_PAGE_SHIFT);
    *addr = (ppn << RV_PAGE_SHIFT) | offset;
}
```

**權限位定義**：
```c
// 訪問類型
access_bits (mmu_translate 參數)：
  - 指令抓取：(1 << 3) = 0b1000 (X 位)
  - 載入操作：(1 << 1) = 0b0010 (R 位)
  - 儲存操作：(1 << 2) = 0b0100 (W 位)

// 設置位
set_bits (mmu_translate 參數)：
  - (1 << 6) = 0b01000000 (A 位，已訪問)
  - (1 << 7) = 0b10000000 (D 位，已修改，僅寫操作)
```

---

## 🚀 TLB 快取實現（核心優化）

### 1. 快取架構設計

SEMU 使用**三個獨立的快取**：

```
hart_t {
    // 指令抓取快取（直接映射）
    mmu_fetch_cache_t cache_fetch;

    // 載入操作快取（8組×2路組相聯）
    mmu_cache_set_t cache_load[8];

    // 儲存操作快取（8組×2路組相聯）
    mmu_cache_set_t cache_store[8];
}
```

**為何分開？**
1. **指令與數據分離**：提高並行性
2. **不同訪問模式**：指令訪問有序，數據訪問隨機
3. **優化各自特性**：指令用直接映射，數據用組相聯

### 2. 指令抓取快取（直接映射）

**結構**：[riscv.h](../riscv.h):34-42

```c
typedef struct {
    uint32_t n_pages;      // 當前快取的 VPN
    uint32_t *page_addr;   // 🔥 主機記憶體指針（零拷貝）
#ifdef MMU_CACHE_STATS
    uint64_t hits;
    uint64_t misses;
#endif
} mmu_fetch_cache_t;
```

**查詢流程**：[riscv.c](../riscv.c):362-386

```c
static void mmu_fetch(hart_t *vm, uint32_t addr, uint32_t *value)
{
    uint32_t vpn = addr >> RV_PAGE_SHIFT;

    // ═══════════════════════════════════════════
    // 快速路徑：快取命中
    // ═══════════════════════════════════════════
    if (likely(vpn == vm->cache_fetch.n_pages)) {
#ifdef MMU_CACHE_STATS
        vm->cache_fetch.hits++;
#endif
        // 直接從主機記憶體讀取（零拷貝）
        uint32_t word_offset = (addr >> 2) & MASK(RV_PAGE_SHIFT - 2);
        *value = vm->cache_fetch.page_addr[word_offset];
        return;
    }

    // ═══════════════════════════════════════════
    // 慢速路徑：快取未命中
    // ═══════════════════════════════════════════
#ifdef MMU_CACHE_STATS
    vm->cache_fetch.misses++;
#endif

    // 虛擬地址轉譯
    mmu_translate(vm, &addr,
                  (1 << 3),  // 需要 X 權限
                  (1 << 6),  // 設置 A 位
                  false,     // 不跳過特權檢查
                  RV_EXC_FETCH_FAULT,
                  RV_EXC_FETCH_PFAULT);
    if (vm->error)
        return;

    // 取得主機記憶體指針
    uint32_t *page_addr;
    vm->mem_fetch(vm, addr >> RV_PAGE_SHIFT, &page_addr);
    if (vm->error)
        return;

    // 更新快取
    vm->cache_fetch.n_pages = vpn;
    vm->cache_fetch.page_addr = page_addr;

    // 讀取值
    uint32_t word_offset = (addr >> 2) & MASK(RV_PAGE_SHIFT - 2);
    *value = page_addr[word_offset];
}
```

**為何使用主機指針？**
```
傳統方式（每次都轉譯）：
  VA → MMU → PA → mem_load(PA)

快取方式（零拷貝）：
  VA → cache_fetch.page_addr[offset]
      └─ 直接訪問主機記憶體
```

### 3. 數據快取（8×2 組相聯）

#### 快取結構

**位置**：[riscv.h](../riscv.h):44-58

```c
/* 單個快取項（一路） */
typedef struct {
    uint32_t n_pages;     // VPN（標籤）
    uint32_t phys_ppn;    // 物理頁號（不是指針！）
#ifdef MMU_CACHE_STATS
    uint64_t hits;
    uint64_t misses;
#endif
} mmu_addr_cache_t;

/* 一個組（2 路） */
typedef struct {
    mmu_addr_cache_t ways[2];  // 兩路
    uint8_t lru;               // LRU 位：0 或 1
} mmu_cache_set_t;

/* hart_t 中有 8 個組 */
mmu_cache_set_t cache_load[8];
mmu_cache_set_t cache_store[8];
```

#### 快取索引計算（3-bit parity hash）

**位置**：[riscv.c](../riscv.c):396-399

```c
uint32_t set_idx = (__builtin_parity(vpn & 0xAAAAAAAA) << 2) |
                   (__builtin_parity(vpn & 0x55555555) << 1) |
                   __builtin_parity(vpn & 0xCCCCCCCC);
```

**Parity Hash 原理**：

```
VPN = 虛擬頁號（20 位）

計算三個 parity 位：
  bit 2 = parity(vpn & 0xAAAAAAAA)  // 偶數位
  bit 1 = parity(vpn & 0x55555555)  // 奇數位
  bit 0 = parity(vpn & 0xCCCCCCCC)  // 特定模式

__builtin_parity(x)：計算 x 的所有位元 XOR
  例如：__builtin_parity(0b1011) = 1^0^1^1 = 1
```

**為何使用 parity hash？**

```
簡單取低 3 位的問題：
  VPN = 0x12340  → set_idx = 0
  VPN = 0x12341  → set_idx = 1
  VPN = 0x12342  → set_idx = 2
  連續頁面映射到連續組，容易衝突

Parity hash 的優勢：
  VPN = 0x12340  → parity → set_idx = 5
  VPN = 0x12341  → parity → set_idx = 2
  VPN = 0x12342  → parity → set_idx = 7
  分散更均勻！
```

**範例計算**：

```
VPN = 0x12345 (二進制: 0001 0010 0011 0100 0101)

Step 1: 提取偶數位（0xAAAAAAAA = 10101010...）
  vpn & 0xAAAAAAAA = 0x00020004
  __builtin_parity(0x00020004) = 0  (有偶數個 1)

Step 2: 提取奇數位（0x55555555 = 01010101...）
  vpn & 0x55555555 = 0x00010001
  __builtin_parity(0x00010001) = 0

Step 3: 特定模式（0xCCCCCCCC = 11001100...）
  vpn & 0xCCCCCCCC = 0x00020004
  __builtin_parity(0x00020004) = 0

結果：set_idx = (0 << 2) | (0 << 1) | 0 = 0
```

#### 載入操作快取查詢

**位置**：[riscv.c](../riscv.c):388-447

```c
static void mmu_load(hart_t *vm,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value,
                     bool reserved)
{
    uint32_t vpn = addr >> RV_PAGE_SHIFT;
    uint32_t phys_addr;

    // ═══════════════════════════════════════════
    // 步驟 1：計算組索引
    // ═══════════════════════════════════════════
    uint32_t set_idx = (__builtin_parity(vpn & 0xAAAAAAAA) << 2) |
                       (__builtin_parity(vpn & 0x55555555) << 1) |
                       __builtin_parity(vpn & 0xCCCCCCCC);

    mmu_cache_set_t *set = &vm->cache_load[set_idx];

    // ═══════════════════════════════════════════
    // 步驟 2：檢查兩路
    // ═══════════════════════════════════════════
    int hit_way = -1;
    for (int way = 0; way < 2; way++) {
        if (likely(set->ways[way].n_pages == vpn)) {
            hit_way = way;
            break;
        }
    }

    // ═══════════════════════════════════════════
    // 步驟 3：快取命中
    // ═══════════════════════════════════════════
    if (likely(hit_way >= 0)) {
#ifdef MMU_CACHE_STATS
        set->ways[hit_way].hits++;
#endif
        // 從快取重建物理地址
        phys_addr = (set->ways[hit_way].phys_ppn << RV_PAGE_SHIFT) |
                    (addr & MASK(RV_PAGE_SHIFT));

        // 更新 LRU：標記另一路為下次替換目標
        set->lru = 1 - hit_way;
    }
    // ═══════════════════════════════════════════
    // 步驟 4：快取未命中
    // ═══════════════════════════════════════════
    else {
        int victim_way = set->lru;  // 使用 LRU 選擇替換路
#ifdef MMU_CACHE_STATS
        set->ways[victim_way].misses++;
#endif

        // 執行完整的 MMU 轉譯
        phys_addr = addr;
        mmu_translate(vm, &phys_addr,
                      (1 << 1) | (vm->sstatus_mxr ? (1 << 3) : 0),  // R 或 R+X
                      (1 << 6),  // 設置 A 位
                      vm->sstatus_sum && vm->s_mode,
                      RV_EXC_LOAD_FAULT,
                      RV_EXC_LOAD_PFAULT);
        if (vm->error)
            return;

        // 更新快取（替換 victim）
        set->ways[victim_way].n_pages = vpn;
        set->ways[victim_way].phys_ppn = phys_addr >> RV_PAGE_SHIFT;

        // 更新 LRU：標記另一路為下次替換目標
        set->lru = 1 - victim_way;
    }

    // ═══════════════════════════════════════════
    // 步驟 5：執行實際記憶體訪問
    // ═══════════════════════════════════════════
    vm->mem_load(vm, phys_addr, width, value);
    if (vm->error)
        return;

    // 如果是 LR 指令，設置保留位
    if (unlikely(reserved))
        vm->lr_reservation = phys_addr | 1;
}
```

**LRU 替換策略**：

```
初始狀態：
  set->ways[0].n_pages = 0xFFFFFFFF  (無效)
  set->ways[1].n_pages = 0xFFFFFFFF  (無效)
  set->lru = 0  (下次替換 way 0)

第一次訪問 VPN=0x100：
  未命中，選擇 way 0 填充
  set->ways[0].n_pages = 0x100
  set->lru = 1  (標記 way 1 為下次替換目標)

第二次訪問 VPN=0x200：
  未命中，選擇 way 1 填充
  set->ways[1].n_pages = 0x200
  set->lru = 0  (標記 way 0 為下次替換目標)

第三次訪問 VPN=0x100：
  命中 way 0
  set->lru = 1  (標記 way 1 為下次替換目標)

第四次訪問 VPN=0x300：
  未命中，選擇 way 1 替換（因為 lru=1）
  set->ways[1].n_pages = 0x300
  set->lru = 0
```

### 4. TLB 失效機制

#### 完全失效

**位置**：[riscv.c](../riscv.c):185-200

```c
void mmu_invalidate(hart_t *vm)
{
    // 失效指令快取
    vm->cache_fetch.n_pages = 0xFFFFFFFF;

    // 失效所有載入快取（8組×2路）
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++)
            vm->cache_load[set].ways[way].n_pages = 0xFFFFFFFF;
        vm->cache_load[set].lru = 0;
    }

    // 失效所有儲存快取（8組×2路）
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++)
            vm->cache_store[set].ways[way].n_pages = 0xFFFFFFFF;
        vm->cache_store[set].lru = 0;
    }
}
```

**觸發時機**：
- 寫入 `satp` 暫存器（切換頁表）
- 執行 `SFENCE.VMA` 指令（無參數）
- SBI `RFENCE` 呼叫（`size=0` 或 `size=-1`）

#### 範圍失效（優化）

**位置**：[riscv.c](../riscv.c):202-251

```c
void mmu_invalidate_range(hart_t *vm, uint32_t start_addr, uint32_t size)
{
    // 特殊情況：size=0 或 -1 表示完全失效
    if (size == 0 || size == (uint32_t) -1) {
        mmu_invalidate(vm);
        return;
    }

    // ═══════════════════════════════════════════
    // 計算 VPN 範圍（防止溢出）
    // ═══════════════════════════════════════════
    uint32_t start_vpn = start_addr >> RV_PAGE_SHIFT;

    // 使用 64 位元防止溢出
    uint64_t end_addr = (uint64_t) start_addr + size - 1;
    if (end_addr > UINT32_MAX)
        end_addr = UINT32_MAX;
    uint32_t end_vpn = (uint32_t) end_addr >> RV_PAGE_SHIFT;

    // ═══════════════════════════════════════════
    // 失效指令快取
    // ═══════════════════════════════════════════
    if (vm->cache_fetch.n_pages >= start_vpn &&
        vm->cache_fetch.n_pages <= end_vpn)
        vm->cache_fetch.n_pages = 0xFFFFFFFF;

    // ═══════════════════════════════════════════
    // 失效載入快取（檢查所有組的所有路）
    // ═══════════════════════════════════════════
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++) {
            if (vm->cache_load[set].ways[way].n_pages >= start_vpn &&
                vm->cache_load[set].ways[way].n_pages <= end_vpn)
                vm->cache_load[set].ways[way].n_pages = 0xFFFFFFFF;
        }
    }

    // ═══════════════════════════════════════════
    // 失效儲存快取
    // ═══════════════════════════════════════════
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++) {
            if (vm->cache_store[set].ways[way].n_pages >= start_vpn &&
                vm->cache_store[set].ways[way].n_pages <= end_vpn)
                vm->cache_store[set].ways[way].n_pages = 0xFFFFFFFF;
        }
    }
}
```

**優化意義**：

```
完全失效：
  - 失效所有 TLB 項（17 項：1 指令 + 8×2 載入 + 8×2 儲存）
  - 下次訪問所有頁面都會未命中

範圍失效：
  - 只失效指定範圍的 VPN
  - 其他頁面的快取項保留
  - 顯著減少 TLB 未命中

範例：
  Linux 修改 1 個頁面（4KB）
  - 完全失效：下 17 次訪問都未命中
  - 範圍失效：只有該頁面未命中，其他命中
```

---

## 🎮 指令執行流程

### 1. vm_step() 主循環

**位置**：[riscv.c](../riscv.c):978-1074

```c
void vm_step(hart_t *vm)
{
    // ═══════════════════════════════════════════
    // 前置檢查
    // ═══════════════════════════════════════════

    // 檢查 hart 是否啟動
    if (vm->hsm_status != SBI_HSM_STATE_STARTED)
        return;

    // 檢查是否有未處理的錯誤
    if (unlikely(vm->error))
        return;

    // 保存當前 PC（用於異常報告）
    vm->current_pc = vm->pc;

    // ═══════════════════════════════════════════
    // 中斷檢查
    // ═══════════════════════════════════════════
    if ((vm->sstatus_sie || !vm->s_mode) && (vm->sip & vm->sie)) {
        uint32_t applicable = vm->sip & vm->sie;
        uint8_t idx = ilog2(applicable);  // 找到最高優先級中斷

        // 特殊處理：S-mode 軟體中斷（清除 SSWI）
        if (idx == 1) {
            emu_state_t *data = PRIV(vm);
            data->sswi.ssip[vm->mhartid] = 0;
        }

        vm->exc_cause = (1U << 31) | idx;  // 設置中斷位
        vm->stval = 0;
        hart_trap(vm);  // 進入陷阱處理
    }

    // ═══════════════════════════════════════════
    // 指令抓取
    // ═══════════════════════════════════════════
    uint32_t insn;
    if (vm->page_table)
        mmu_fetch(vm, vm->pc, &insn);  // 啟用 MMU
    else
        vm->mem_fetch(vm, vm->pc >> RV_PAGE_SHIFT, &insn);  // 物理模式

    if (vm->error)
        return;

    // PC 提前更新（大多數指令）
    vm->pc += 4;

    // ═══════════════════════════════════════════
    // 指令解碼與執行
    // ═══════════════════════════════════════════
    const uint8_t opcode = insn & 0x7F;

    switch (opcode) {
    // ... (各種指令的處理，下面詳細說明)
    }
}
```

### 2. 各類指令實現

#### R-type（暫存器運算）

```c
case RV32_OP: {  // 0b0110011
    uint32_t funct7 = insn >> 25;

    if (funct7 == 0b0000001) {
        // RV32M 擴展：乘法/除法
        set_dest(vm, insn, op_mul(insn, read_rs1(vm, insn), read_rs2(vm, insn)));
    } else {
        // RV32I 基本運算
        set_dest(vm, insn, op_rv32i(insn, true, read_rs1(vm, insn), read_rs2(vm, insn)));
    }
    break;
}
```

**op_rv32i() 實現**：[riscv.c](../riscv.c):840-863

```c
static uint32_t op_rv32i(uint32_t insn, bool is_reg, uint32_t a, uint32_t b)
{
    uint8_t funct3 = decode_func3(insn);

    switch (funct3) {
    case 0b000:  // ADD / SUB
        if (is_reg && (insn & (1 << 30)))  // SUB（檢查 bit 30）
            return a - b;
        else
            return a + b;

    case 0b001:  // SLL（邏輯左移）
        return a << (b & 0x1F);

    case 0b010:  // SLT（有符號比較）
        return ((int32_t) a) < ((int32_t) b);

    case 0b011:  // SLTU（無符號比較）
        return a < b;

    case 0b100:  // XOR
        return a ^ b;

    case 0b101:  // SRL / SRA
        if (insn & (1 << 30))  // SRA（算術右移）
            return (uint32_t) (((int32_t) a) >> (b & 0x1F));
        else  // SRL（邏輯右移）
            return a >> (b & 0x1F);

    case 0b110:  // OR
        return a | b;

    case 0b111:  // AND
        return a & b;
    }
}
```

#### I-type（立即數運算）

```c
case RV32_OP_IMM: {  // 0b0010011
    uint32_t imm = decode_i(insn);
    set_dest(vm, insn, op_rv32i(insn, false, read_rs1(vm, insn), imm));
    break;
}
```

**範例**：`ADDI x5, x10, -100`
```
1. decode_i(insn) → imm = -100
2. read_rs1(vm, insn) → a = x_regs[10]
3. op_rv32i(..., false, a, -100) → a + (-100)
4. set_dest(vm, insn, result) → x_regs[5] = result
```

#### Load/Store 指令

```c
case RV32_LOAD: {  // 0b0000011
    uint32_t addr = read_rs1(vm, insn) + decode_i(insn);
    uint32_t value;

    if (vm->page_table)
        mmu_load(vm, addr, decode_func3(insn), &value, false);
    else
        vm->mem_load(vm, addr, decode_func3(insn), &value);

    if (vm->error)
        return;

    set_dest(vm, insn, value);
    break;
}

case RV32_STORE: {  // 0b0100011
    uint32_t addr = read_rs1(vm, insn) + decode_s(insn);
    uint32_t value = read_rs2(vm, insn);

    if (vm->page_table)
        mmu_store(vm, addr, decode_func3(insn), value, false);
    else
        vm->mem_store(vm, addr, decode_func3(insn), value);

    break;
}
```

**寬度編碼**（funct3）：
```c
enum {
    RV_MEM_LB  = 0b000,  // 載入  byte（符號擴展）
    RV_MEM_LH  = 0b001,  // 載入 halfword（符號擴展）
    RV_MEM_LW  = 0b010,  // 載入  word
    RV_MEM_LBU = 0b100,  // 載入  byte（零擴展）
    RV_MEM_LHU = 0b101,  // 載入 halfword（零擴展）
    RV_MEM_SB  = 0b000,  // 儲存  byte
    RV_MEM_SH  = 0b001,  // 儲存 halfword
    RV_MEM_SW  = 0b010,  // 儲存  word
};
```

#### 分支指令

```c
case RV32_BRANCH: {  // 0b1100011
    if (op_jmp(vm, insn, read_rs1(vm, insn), read_rs2(vm, insn))) {
        // 分支成立
        uint32_t target = vm->current_pc + decode_b(insn);
        do_jump(vm, target);
    }
    // 否則 PC 已經 +4，繼續下一條指令
    break;
}
```

**op_jmp() 實現**：[riscv.c](../riscv.c):866-884

```c
static bool op_jmp(hart_t *vm, uint32_t insn, uint32_t a, uint32_t b)
{
    switch (decode_func3(insn)) {
    case 0b000:  // BEQ
        return a == b;
    case 0b001:  // BNE
        return a != b;
    case 0b100:  // BLT（有符號）
        return ((int32_t) a) < ((int32_t) b);
    case 0b101:  // BGE（有符號）
        return ((int32_t) a) >= ((int32_t) b);
    case 0b110:  // BLTU（無符號）
        return a < b;
    case 0b111:  // BGEU（無符號）
        return a >= b;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return false;
    }
}
```

#### 跳轉指令

```c
case RV32_JAL: {  // 0b1101111
    // JAL: 跳轉並連結
    uint32_t target = vm->current_pc + decode_j(insn);
    op_jump_link(vm, insn, target);
    break;
}

case RV32_JALR: {  // 0b1100111
    // JALR: 間接跳轉並連結
    uint32_t target = (read_rs1(vm, insn) + decode_i(insn)) & ~1;
    op_jump_link(vm, insn, target);
    break;
}
```

**op_jump_link() 實現**：[riscv.c](../riscv.c):894-902

```c
static void op_jump_link(hart_t *vm, uint32_t insn, uint32_t addr)
{
    if (unlikely(addr & 0b11)) {
        // 目標地址未對齊（必須 4 字節對齊）
        vm_set_exception(vm, RV_EXC_PC_MISALIGN, addr);
    } else {
        set_dest(vm, insn, vm->pc);  // 保存返回地址（PC+4）
        vm->pc = addr;  // 跳轉到目標
    }
}
```

#### 原子操作

**位置**：[riscv.c](../riscv.c):914-971

```c
case RV32_AMO: {  // 0b0101111
    op_amo(vm, insn);
    break;
}
```

**LR/SC 實現**：

```c
static void op_amo(hart_t *vm, uint32_t insn)
{
    uint32_t addr = read_rs1(vm, insn);
    uint32_t funct5 = decode_func5(insn);

    switch (funct5) {
    case 0b00010:  // LR.W（載入並保留）
        if (addr & 0b11)  // 檢查對齊
            return vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, addr);

        uint32_t value;
        mmu_load(vm, addr, RV_MEM_LW, &value, true);  // reserved=true
        if (vm->error)
            return;

        set_dest(vm, insn, value);
        // mmu_load 已經設置 vm->lr_reservation
        break;

    case 0b00011:  // SC.W（條件儲存）
        if (addr & 0b11)
            return vm_set_exception(vm, RV_EXC_STORE_MISALIGN, addr);

        bool ok = mmu_store(vm, addr, RV_MEM_SW, read_rs2(vm, insn), true);
        if (vm->error)
            return;

        set_dest(vm, insn, ok ? 0 : 1);  // 0=成功，1=失敗
        break;

    // 其他原子操作（AMOSWAP、AMOADD 等）
    // ...
    }
}
```

**LR/SC 工作原理**：

```
hart_t {
    uint32_t lr_reservation;  // bit 0 = 有效位，bit 1-31 = 物理地址
}

LR.W x5, (x10):
  1. 讀取 [x10] 的值
  2. 設置 lr_reservation = phys_addr | 1

SC.W x6, x11, (x10):
  1. 檢查 lr_reservation 是否有效（bit 0 = 1）
  2. 檢查地址是否匹配（phys_addr == lr_reservation & ~1）
  3. 如果都滿足：
       - 寫入 [x10] = x11
       - 返回 0（成功）
     否則：
       - 不寫入
       - 返回 1（失敗）
  4. 清除 lr_reservation

用於實現鎖：
    li a0, 1
retry:
    lr.w t0, (lock_addr)     # 載入鎖
    bnez t0, retry           # 如果已鎖定，重試
    sc.w t1, a0, (lock_addr) # 嘗試獲取鎖
    bnez t1, retry           # 如果失敗，重試
    # 臨界區
    sw zero, (lock_addr)     # 釋放鎖
```

---

## 🚨 異常處理

### 1. 異常設置

**位置**：[riscv.c](../riscv.c):

```c
void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t val)
{
    vm->error = ERR_EXCEPTION;
    vm->exc_cause = cause;
    vm->exc_val = val;
}
```

### 2. hart_trap() 陷阱處理

**位置**：[riscv.c](../riscv.c):519-585

```c
void hart_trap(hart_t *vm)
{
    // ═══════════════════════════════════════════
    // 步驟 1：保存當前狀態
    // ═══════════════════════════════════════════

    // 保存異常發生時的 PC
    vm->sepc = vm->current_pc;

    // 保存異常原因和附加值
    vm->scause = vm->exc_cause;
    vm->stval = vm->exc_val;

    // 保存當前特權模式
    vm->sstatus_spp = vm->s_mode;

    // 保存中斷使能狀態
    vm->sstatus_spie = vm->sstatus_sie;

    // ═══════════════════════════════════════════
    // 步驟 2：進入 S-mode
    // ═══════════════════════════════════════════
    vm->s_mode = true;

    // 禁用中斷
    vm->sstatus_sie = false;

    // ═══════════════════════════════════════════
    // 步驟 3：跳轉到異常處理程序
    // ═══════════════════════════════════════════
    if (vm->stvec_vectored && (vm->scause & (1U << 31))) {
        // 向量化模式：不同中斷跳轉到不同地址
        uint32_t cause_idx = vm->scause & 0x7FFFFFFF;
        vm->pc = vm->stvec_addr + (cause_idx << 2);
    } else {
        // 直接模式：所有異常跳轉到同一地址
        vm->pc = vm->stvec_addr;
    }

    // 清除錯誤狀態
    vm->error = ERR_NONE;
}
```

**陷阱處理流程圖**：

```
異常發生
  ↓
vm_set_exception(cause, val)
  ├─ vm->error = ERR_EXCEPTION
  ├─ vm->exc_cause = cause
  └─ vm->exc_val = val
  ↓
vm_step() 返回到 main.c
  ↓
main.c 檢查 hart->error
  ↓
if (exc_cause == ECALL_S)
  → handle_sbi_ecall()  // SBI 處理
else
  → hart_trap()  // 委派給 S-mode
  ↓
  保存狀態：
    sepc = current_pc
    scause = exc_cause
    stval = exc_val
    sstatus_spp = s_mode
    sstatus_spie = sstatus_sie
  ↓
  進入 S-mode：
    s_mode = true
    sstatus_sie = false
  ↓
  跳轉：
    pc = stvec_addr (+ offset)
  ↓
  Linux 內核異常處理程序
    ↓
    執行 SRET 返回
      ↓
      恢復狀態：
        pc = sepc
        s_mode = sstatus_spp
        sstatus_sie = sstatus_spie
```

### 3. CSR 操作

**CSR 讀取**：[riscv.c](../riscv.c):587-650

```c
static uint32_t csr_read(hart_t *vm, uint16_t csr)
{
    switch (csr) {
    // ═══════════════════════════════════════════
    // 無特權 CSR
    // ═══════════════════════════════════════════
    case RV_CSR_TIME:
        return vm->time & 0xFFFFFFFF;  // 低 32 位
    case RV_CSR_TIMEH:
        return vm->time >> 32;  // 高 32 位
    case RV_CSR_INSTRET:
        return vm->instret & 0xFFFFFFFF;
    case RV_CSR_INSTRETH:
        return vm->instret >> 32;

    // ═══════════════════════════════════════════
    // S-mode CSR
    // ═══════════════════════════════════════════
    case RV_CSR_SSTATUS:
        return (vm->sstatus_sie << 1) |
               (vm->sstatus_spie << 5) |
               (vm->sstatus_spp << 8) |
               (vm->sstatus_sum << 18) |
               (vm->sstatus_mxr << 19);

    case RV_CSR_SIE:
        return vm->sie;

    case RV_CSR_STVEC:
        return vm->stvec_addr | (vm->stvec_vectored ? 1 : 0);

    case RV_CSR_SEPC:
        return vm->sepc;

    case RV_CSR_SCAUSE:
        return vm->scause;

    case RV_CSR_STVAL:
        return vm->stval;

    case RV_CSR_SIP:
        return vm->sip;

    case RV_CSR_SATP:
        return vm->satp;

    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return 0;
    }
}
```

**CSR 寫入**：[riscv.c](../riscv.c):652-689

```c
static void csr_write(hart_t *vm, uint16_t csr, uint32_t value)
{
    switch (csr) {
    case RV_CSR_SSTATUS:
        vm->sstatus_sie = (value >> 1) & 1;
        vm->sstatus_spie = (value >> 5) & 1;
        vm->sstatus_spp = (value >> 8) & 1;
        vm->sstatus_sum = (value >> 18) & 1;
        vm->sstatus_mxr = (value >> 19) & 1;
        break;

    case RV_CSR_SIE:
        vm->sie = value & 0x222;  // 只保留 SSI/STI/SEI 位
        break;

    case RV_CSR_STVEC:
        vm->stvec_addr = value & ~3;  // 對齊到 4 字節
        vm->stvec_vectored = value & 1;
        break;

    case RV_CSR_SEPC:
        vm->sepc = value;
        break;

    case RV_CSR_SCAUSE:
        vm->scause = value;
        break;

    case RV_CSR_STVAL:
        vm->stval = value;
        break;

    case RV_CSR_SIP:
        vm->sip = (vm->sip & ~0x222) | (value & 0x222);
        break;

    case RV_CSR_SATP:
        mmu_set(vm, value);  // 設置頁表並失效 TLB
        break;

    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    }
}
```

**CSR 指令**：

```c
case RV32_SYSTEM: {
    uint32_t funct3 = decode_func3(insn);
    uint16_t csr = insn >> 20;

    switch (funct3) {
    case 0b001:  // CSRRW（讀寫）
        {
            uint32_t tmp = csr_read(vm, csr);
            csr_write(vm, csr, read_rs1(vm, insn));
            set_dest(vm, insn, tmp);
        }
        break;

    case 0b010:  // CSRRS（讀並設置位）
        {
            uint32_t tmp = csr_read(vm, csr);
            csr_write(vm, csr, tmp | read_rs1(vm, insn));
            set_dest(vm, insn, tmp);
        }
        break;

    case 0b011:  // CSRRC（讀並清除位）
        {
            uint32_t tmp = csr_read(vm, csr);
            csr_write(vm, csr, tmp & ~read_rs1(vm, insn));
            set_dest(vm, insn, tmp);
        }
        break;

    // ... 立即數版本（CSRRWI/CSRRSI/CSRRCI）
    }
    break;
}
```

---

## 🎓 實踐練習

### 練習 1：追蹤指令執行

**目標**：使用 GDB 觀察一條 ADDI 指令的完整執行過程

```bash
# 啟動 SEMU（GDB 模式）
./build/semu -k build/Image -g 1234 &

# 啟動 GDB
riscv32-unknown-elf-gdb
(gdb) target remote :1234
(gdb) break vm_step
(gdb) continue

# 到達斷點後
(gdb) print/x $pc
(gdb) x/i $pc          # 顯示指令
(gdb) step             # 單步執行
(gdb) print vm->x_regs[5]  # 查看結果暫存器
```

### 練習 2：觀察 MMU 轉譯

**目標**：追蹤一次虛擬地址轉譯

```bash
(gdb) break mmu_translate
(gdb) continue

# 到達斷點
(gdb) print/x *addr          # 虛擬地址
(gdb) print/x vm->satp       # 頁表基地址
(gdb) step                   # 執行 mmu_lookup
(gdb) print/x *addr          # 物理地址（轉譯後）
```

### 練習 3：測試 TLB 快取

**目標**：觀察 TLB 命中和未命中

```c
// 編譯時啟用統計
#define MMU_CACHE_STATS

// 在 Linux 中執行
cat /proc/meminfo  # 訪問大量頁面

// 發送信號查看統計
kill -SIGUSR1 <semu_pid>

// 輸出範例：
// Load cache:
//   Set 0: way 0 hits=1234 misses=56, way 1 hits=789 misses=23
//   Set 1: way 0 hits=2345 misses=12, way 1 hits=456 misses=78
//   ...
```

### 練習 4：觸發頁面錯誤

**目標**：故意觸發頁面錯誤並觀察處理過程

```c
// 在 Linux 中
int *ptr = (int *)0xDEADBEEF;  // 未映射的地址
*ptr = 42;  // 觸發 Store Page Fault

// GDB 追蹤：
(gdb) break mmu_translate
(gdb) continue
(gdb) print/x *addr       # 0xDEADBEEF
(gdb) continue            # 頁表遍歷失敗
(gdb) break hart_trap
(gdb) continue
(gdb) print vm->scause    # RV_EXC_STORE_PFAULT (15)
```

### 練習 5：分析指令混合

**目標**：統計不同指令類型的比例

```bash
# 使用 perf 或自定義計數器
# 在 vm_step() 中添加：
static uint64_t insn_count[128] = {0};
insn_count[opcode]++;

// 定期輸出統計
printf("OP_IMM: %lu\n", insn_count[RV32_OP_IMM]);
printf("LOAD:   %lu\n", insn_count[RV32_LOAD]);
printf("STORE:  %lu\n", insn_count[RV32_STORE]);
printf("BRANCH: %lu\n", insn_count[RV32_BRANCH]);
```

---

## 📊 知識檢查點

### 基礎問題

1. **Q**: RV32IMA 中各字母代表什麼？
   **A**: I=基本整數，M=乘法/除法，A=原子操作

2. **Q**: RISC-V 有幾種指令格式？
   **A**: 6 種（R、I、S、B、U、J）

3. **Q**: Sv32 有幾級頁表？
   **A**: 2 級（Level 1 + Level 0）

4. **Q**: 每個頁面多大？
   **A**: 4KB (2^12 bytes)

5. **Q**: TLB 使用什麼快取策略？
   **A**: 8×2 組相聯（8 組，每組 2 路）

### 進階問題

1. **Q**: 為何使用 parity hash 而非簡單取低位？
   **A**: 分散更均勻，減少連續頁面的衝突

2. **Q**: LRU 位如何工作？
   **A**: 記錄下次替換哪一路（0 或 1），每次命中時更新為另一路

3. **Q**: 指令快取為何儲存主機指針而不是 PPN？
   **A**: 零拷貝優化，直接訪問主機記憶體，避免額外轉換

4. **Q**: mmu_invalidate_range 如何防止溢出？
   **A**: 使用 64 位元計算 end_addr，然後鉗位到 UINT32_MAX

5. **Q**: PTE 的哪些位組合是非法的？
   **A**: V=1 且 R=W=X=0 以外的組合（如 V=0, R=1）

### 深入問題

1. **Q**: 為何 J-type 立即數的排列順序是亂的？
   **A**: 硬體優化，減少關鍵路徑延遲（符號位直接在 MSB）

2. **Q**: LR/SC 如何實現鎖？
   **A**: LR 保留地址，SC 檢查保留是否被打破，形成原子 read-modify-write

3. **Q**: hart_trap() 為何要禁用中斷？
   **A**: 防止異常處理中被中斷打斷，保證原子性

4. **Q**: 大頁（superpage）有什麼限制？
   **A**: PPN[0] 必須為 0（對齊到 4MB 邊界）

5. **Q**: 為何 store 快取也需要 LRU？
   **A**: 寫操作也可能頻繁訪問相同頁面，快取提升性能

---

## 🎯 下一階段預告

完成本階段後，你已經掌握了 RISC-V CPU 的核心機制。

**階段 3** 將深入研究：
- SBI 0.2 標準的完整實現
- HSM 多核心管理機制
- IPI 處理器間中斷
- RFENCE 遠程柵欄
- 協程式 SMP 調度系統

繼續閱讀：[階段 3：系統服務層](./stage3-system-services.md)

---

**文檔版本**：v1.0
**最後更新**：2025-01-10
**預估學習時間**：3-4 天
