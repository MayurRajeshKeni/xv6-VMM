# xv6-VMM Project Report: Demand Paging with Swap

---

## 1. OS Choice & Scope

### 1.1 Why xv6?

**xv6** is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix Version 6, developed at MIT for teaching purposes. We chose xv6 for the following reasons:

| Criterion | Why xv6 is Ideal |
|---|---|
| **Small & Lightweight** | The entire kernel is ~10,000 lines of C — small enough to fully understand in a semester, yet complete enough to be a real OS with processes, virtual memory, a file system, and a disk driver. |
| **Well-Suited to the Timeframe** | Unlike modifying Linux (28+ million lines), xv6 lets each team member own a specific subsystem end-to-end within weeks. |
| **Real Hardware Abstractions** | xv6 uses x86 two-level page tables, PIO-based IDE disk I/O, and a buffer cache — the exact components a demand-paging project must touch. |
| **No Existing Swap Support** | The baseline xv6 has **zero** swap/paging infrastructure, so every line we wrote is a genuine kernel modification, not a config change. |

### 1.2 Planned Modification — Precisely Defined

We implemented a **demand paging system with disk-backed swap** in three cleanly separated modules:

| Module | Owner | Kernel Files Modified | Purpose |
|---|---|---|---|
| **Disk & Swap Subsystem** | Mayur Rajesh Keni | `param.h`, `fs.h`, `ide.c`, `mkfs.c`, `defs.h` | Define swap boundaries on disk; build data structures to track swap slot allocation; implement `swap_write` / `swap_read` to move 4KB pages across 8 disk sectors. |
| **Page Fault Handler** | Hari-Prannav | `trap.c`, `vm.c` | Intercept `T_PGFLT` (trap 14) in `trap()`, walk the page table to detect swapped-out pages, call `swap_read` to bring them back, and remap the PTE. |
| **Eviction Policy & Memory Capping** | Nitin-YK | `vm.c`, `proc.h`, `kalloc.c` | Enforce a per-process physical page cap; implement a clock/FIFO eviction policy that selects a victim page, calls `swap_write`, clears the PTE, and frees the physical frame. |

> [!IMPORTANT]
> The three modules form a closed loop: **Eviction** calls `swap_write` → **Disk Subsystem** persists the page → **Page Fault Handler** calls `swap_read` → **Disk Subsystem** restores the page → execution resumes transparently.

---

## 2. Environment Setup & Kernel Build

### 2.1 Development Environment

| Component | Version / Details |
|---|---|
| Host OS | Ubuntu (Linux, x86-64) |
| Virtual Machine | QEMU `qemu-system-i386` v10.2.1 |
| Cross-Compiler | `gcc` (i686 target via `-m32` flag) |
| Source Baseline | MIT xv6 (rev11), commit `30b754a` |
| Repository | [github.com/MayurRajeshKeni/xv6-VMM](https://github.com/MayurRajeshKeni/xv6-VMM) |

### 2.2 Baseline Boot Verification

Before any modifications were made, we verified the **unmodified** xv6 kernel builds and boots cleanly:

```
$ make clean && make
$ make qemu-nox
```

**Baseline boot output (clean xv6, commit `30b754a`):**
```
xv6...
cpu0: starting 0
sb: size 1000 nblocks 941 ninodes 200 nlog 30 logstart 2 inodestart 32 bmap start 58
init: starting sh
$ ls
.              1 1 512
..             1 1 512
README         2 2 2286
cat            2 3 16080
echo           2 4 14972
...
```

> [!NOTE]
> The baseline commit (`30b754a`) is preserved in git history. Anyone can `git checkout 30b754a` to reproduce the unmodified build and confirm it boots without errors.

---

## 3. Kernel Modification — Disk & Swap Subsystem (Member 1: Mayur)

### 3.1 Problem Statement

xv6's physical memory is divided into **4KB pages** (`PGSIZE = 4096`), but the IDE disk driver operates in **512-byte sectors** (`BSIZE = 512`). To swap a single memory page to disk, we must translate one 4KB request into **8 consecutive 512-byte sector writes**. Additionally, the swap space must be allocated **after** the file system data to avoid corrupting xv6's existing file system.

### 3.2 Design Decisions

#### 3.2.1 Swap Boundary Placement

The critical design question was: *where on the disk does swap space live?*

We placed swap immediately after `FSSIZE` (block 1000):

```
Disk Layout:
[ boot | super | log | inodes | bitmap | data blocks ] [ swap slots ]
<------------ FSSIZE = 1000 blocks -----------------> <-- 8192 blocks -->
                                                       ^
                                                       SWAP_START = 1000
```

**Rationale:** This is the safest option. The file system never accesses blocks ≥ `FSSIZE`, so the swap space is completely invisible to it. No file system metadata needs modification.

#### 3.2.2 Swap Slot Tracking — Array of Structs vs. Bitmap

We considered two approaches:

| Approach | Pros | Cons |
|---|---|---|
| **Bitmap** (1 bit per slot) | Memory-efficient (128 bytes for 1024 slots) | Cannot store per-slot metadata (permissions); bit manipulation is error-prone |
| **`struct swap_entry` array** | Can store `is_free` flag AND `page_perm` metadata per slot; simpler code | Uses more memory (8KB for 1024 slots) |

We chose the **struct array** because Member 2 and Member 3 need to preserve PTE permission bits (read/write/user) across swap-out and swap-in. The `page_perm` field makes this trivial.

#### 3.2.3 Direct IDE I/O vs. Buffer Cache

We bypass the buffer cache (`bread`/`bwrite`) and use a **stack-allocated `struct buf`** directly with `iderw()`. Rationale:
- Swap pages are written once and read once — caching them wastes buffer cache slots needed by the file system.
- The buffer cache's LRU policy would evict useful FS blocks to make room for swap data that won't be re-read soon.
- Direct I/O is simpler and avoids lock-ordering issues between the buffer cache lock and our swap table lock.

### 3.3 Files Modified & What Changed

#### `param.h` — Swap Space Size
```c
#define MAX_SWAP_PAGES 1024  // swap can hold up to 1024 pages (4MB)
```
**Why 1024?** This provides 4MB of swap space — enough for dozens of processes' working sets in xv6's 224MB physical memory environment, while keeping the `swap_table` struct under 8KB of kernel memory.

#### `fs.h` — Swap Boundary Constants & Data Structure
```c
#define SECTORS_PER_PAGE  (PGSIZE / BSIZE)   // = 8
#define SWAP_START        (FSSIZE)           // = 1000
#define SWAP_SECTORS      (MAX_SWAP_PAGES * SECTORS_PER_PAGE)  // = 8192
#define TOTAL_DISK_BLOCKS (FSSIZE + SWAP_SECTORS)              // = 9192
#define SWAP_SLOT_TO_SECTOR(slot) (SWAP_START + ((slot) * SECTORS_PER_PAGE))

struct swap_entry {
  int is_free;       // 1 = free, 0 = allocated
  uint page_perm;    // preserved PTE flags (PTE_W, PTE_U, etc.)
};
```

#### `ide.c` — Core Swap Implementation (~210 lines added)

| Function | Lines | Purpose |
|---|---|---|
| `swap_init()` | 186–197 | Initialize spinlock and mark all 1024 slots free |
| `swap_alloc()` | 200–215 | Thread-safe first-fit allocation; returns slot index or -1 |
| `swap_free(slot)` | 218–228 | Release a slot; resets `is_free` and `page_perm` |
| `swap_is_free(slot)` | 231–241 | Query whether a slot is available |
| `swap_set_perm(slot, perm)` | 244–253 | Store PTE permission bits for later restoration |
| `swap_get_perm(slot)` | 256–266 | Retrieve stored PTE permission bits |
| `swap_write(page, slot)` | 269–294 | **Core: write 4KB page → 8 sectors** |
| `swap_read(page, slot)` | 297–322 | **Core: read 8 sectors → 4KB page** |
| `swap_test()` | 325–376 | End-to-end self-test with data integrity verification |

**The critical 8-sector loop** in `swap_write`:
```c
for(i = 0; i < SECTORS_PER_PAGE; i++){   // i = 0..7
    b.blockno = start_sector + i;         // sector 1000, 1001, ..., 1007
    b.flags = B_DIRTY;                    // mark for write
    memmove(b.data, addr + (i * BSIZE), BSIZE);  // copy 512 bytes
    iderw(&b);                            // issue IDE write + wait
}
```

#### `ide.c` — `idestart()` Block Limit Fix
```c
// BEFORE: if(b->blockno >= FSSIZE)       ← rejects swap sector numbers!
// AFTER:  if(b->blockno >= TOTAL_DISK_BLOCKS)  ← allows swap sectors
```
**This was a critical obstacle.** Without this change, any swap I/O panics the kernel with "incorrect blockno" because swap sectors (≥1000) exceed `FSSIZE`.

#### `mkfs.c` — Disk Image Expansion
```c
// BEFORE: for(i = 0; i < FSSIZE; i++)        → 512KB disk image
// AFTER:  for(i = 0; i < TOTAL_DISK_BLOCKS; i++)  → 4.5MB disk image
```
This ensures QEMU's virtual IDE disk has physical sectors allocated for the swap region.

#### `defs.h` — API Exposure
Added 9 function prototypes under `// ide.c` so that `trap.c` (Member 2) and `vm.c` (Member 3) can call the swap API.

### 3.4 Obstacles Encountered & Resolutions

| Obstacle | How We Resolved It |
|---|---|
| **`idestart` panicked on swap sector numbers** | Changed the bounds check from `FSSIZE` to `TOTAL_DISK_BLOCKS` |
| **`mkfs` didn't allocate disk space for swap** | Extended the zero-fill loop from `FSSIZE` to `TOTAL_DISK_BLOCKS` |
| **`README` renamed to `README.md` broke `make fs.img`** | Updated the `Makefile` target from `README` to `README.md` |
| **Buffer cache pollution from swap I/O** | Used a stack-allocated `struct buf` instead of `bread`/`bwrite` |
| **Race conditions on swap slot allocation** | Protected `swap_table` with a dedicated spinlock |

### 3.5 Verification

We created a **self-test** (`swaptest` user program + `swap_test()` kernel function) that:
1. Allocates a swap slot (`swap_alloc`)
2. Fills a 4KB page with a known pattern (byte `i % 251 + 1`)
3. Writes the page to disk (`swap_write`) — 8 sector writes
4. Zeroes out the memory page completely
5. Reads the page back from disk (`swap_read`) — 8 sector reads
6. Verifies all 4096 bytes match the original pattern
7. Frees the page and slot

**QEMU output:**
```
$ swaptest
Running swap subsystem test...
swap_test: PASSED! 4KB page write/read verified across 8 disk sectors.
TEST PASSED: Disk swap read/write verified successfully!
```
 
We also verified the file system remained intact by running `ls` and `mkdir` after the swap test.

---

## 4. Kernel Modification — Page Fault Handler (Member 2: Hari-Prannav)

### 4.1 Design & Mechanism
The page fault handler acts as the bridge between user-space memory access and the virtual memory swap subsystem. When a user process attempts to access a virtual address mapped to an unpresent page (`PTE_P == 0`), the x86 CPU automatically triggers exception vector 14 (`T_PGFLT`) and loads the faulting linear address into the `%cr2` control register.

### 4.2 Implementation Details (`trap.c`)
In `trap.c`, `case T_PGFLT:` was added to the interrupt dispatcher:
```c
  case T_PGFLT: {
    // 1. Read the cr2 register to identify the faulting virtual address
    uint fault_addr = rcr2();
    
    // 2. We must ensure a user process is currently running
    if(myproc() == 0){
      panic("T_PGFLT: Page fault in kernel space without a process");
    }

    // 3. Call swapinpage() from vm.c. 
    // This safely handles kalloc(), calling Mayur's swap_read(), and updating 
    // the PTEs while holding the necessary eviction locks.
    if(swapinpage(myproc()->pgdir, fault_addr) < 0){
      // If swapinpage returns -1, it means the address wasn't a valid swapped page.
      // We treat this as a standard segmentation fault and kill the process.
      cprintf("pid %d %s: trap 14 err %d on cpu %d eip 0x%x addr 0x%x -- kill proc\n",
              myproc()->pid, myproc()->name, tf->err, cpuid(), tf->eip, fault_addr);
      myproc()->killed = 1;
    }
    break;
  }
```

### 4.3 Key Handled Scenarios
1. **Valid Swapped-Out Page**: `swapinpage()` identifies the page as swapped (`PTE_SWAPPED`), extracts the swap slot, allocates a physical frame via `kalloc()`, restores the 4KB data using `swap_read()`, updates the PTE with `PTE_P`, and frees the slot. Instruction execution resumes transparently.
2. **Invalid Access (Segmentation Fault)**: If the faulting address does not point to a valid swapped page (e.g., NULL pointer dereference or out-of-bounds address), `swapinpage()` returns `-1`, and the process is killed cleanly.
3. **Protection Faults**: Guarded against infinite trap loops on present-but-forbidden pages.

---

## 5. Kernel Modification — Eviction Policy & Memory Capping (Member 3: Nitin-YK)

### 5.1 Design & Memory Capping (`swap.h`, `kalloc.c`, `vm.c`)
To simulate real-world memory pressure and enforce demand paging in xv6, a physical memory cap was introduced in `swap.h`:
```c
#define PHYSCAP        (6*1024*1024)   // 6 MB memory cap
#define PTE_SWAPPED    0x200          // Software-available bit 9 in x86 PTE
#define PTE_A          0x020          // Accessed bit (set by CPU hardware)
#define PTE_SLOT(pte)  ((uint)(pte) >> 12)
```

In `kalloc.c`, when free physical memory is exhausted or the system approaches `PHYSCAP`, `kalloc()` triggers the page eviction engine to reclaim physical frames before memory allocations fail.

### 5.2 Clock Page Replacement Algorithm (`vm.c`)
A **Clock (Second-Chance) Page Replacement Algorithm** was implemented to select victim pages:
1. The evictor inspects candidate user pages in a circular queue.
2. If the page's Accessed bit (`PTE_A`) is set (`1`), the CPU has accessed the page recently. The evictor clears `PTE_A` to `0` (giving it a second chance) and advances the clock hand.
3. If `PTE_A` is already `0`, the page has not been accessed since the last clock sweep. It is chosen as the victim.
4. The evictor calls Mayur's `swap_alloc()` to reserve a disk slot, writes the 4KB memory page to disk using `swap_write()`, marks the PTE as unpresent (`PTE_P = 0`, `PTE_SWAPPED = 1`, storing the slot number in bits 31..12), flushes the TLB, and frees the physical frame for reuse.

### 5.3 Verification Test Programs
- **`memtest.c`**: Allocates 2,048 pages (8 MB) — exceeding the 6 MB `PHYSCAP`. Verifies that older pages are evicted to swap space and transparently paged back in upon verification without data loss.
- **`forkswap.c`**: Allocates 1,536 pages (6 MB) beyond the memory cap, forks a child process, and verifies copy-on-write / `copyuvm` memory duplication correctly preserves swapped-out pages across both parent and child.

---

## 6. What We Learned

| Team Member | Key Takeaway |
|---|---|
| **Mayur Rajesh Keni** | Understanding how the IDE disk driver operates at the PIO register level (ports 0x1f0–0x1f7) and decomposing a 4KB memory page into 8 consecutive 512-byte sector operations that properly interface with xv6's asynchronous interrupt request (`IRQ_IDE`) mechanism without corrupting the file system. |
| **Hari-Prannav** | Mastering the x86 trap/interrupt handling architecture, specifically intercepting exception 14 (`T_PGFLT`), extracting the faulting linear address from control register `%cr2`, and distinguishing between true segmentation faults and transparent swapped-out demand paging traps. |
| **Nitin-YK** | Designing memory pressure detection and implementing the Clock eviction algorithm in the kernel, tracking hardware-maintained accessed bits (`PTE_A`), encoding disk slot descriptors within unused PTE bits, and coordinating thread-safe frame reclamation. |

---

## 7. References

1. Cox, R., Kaashoek, F., & Morris, R. — *xv6: a simple, Unix-like teaching operating system* (MIT, 2006–2024)
2. xv6 source code — [github.com/mit-pdos/xv6-public](https://github.com/mit-pdos/xv6-public)
3. Intel 80386 Programmer's Reference Manual — Chapter 9: Exceptions and Interrupts
4. Silberschatz, A., Galvin, P., & Gagne, G. — *Operating System Concepts*, 10th Edition — Chapter 10: Virtual Memory

---

## Appendix A: Git Commit History

```
e352f24  Merge branch 'hari-page-fault-handler' into main
372610f  feat: Integrated T_PGFLT handler with swapinpage (Hari-Prannav)
ac834e3  docs: Replace rubric image with clearer high-resolution version
aeadc46  docs: Add comprehensive README and organize docs directory with project report and video script
8e0b771  Merge pull request #1 from MayurRajeshKeni/nitin-eviction
dfd5f24  Add minimal T_PGFLT handler calling swapinpage (Nitin-YK)
5656cde  Add clock eviction, memory cap, swapinpage; raise MAX_SWAP_PAGES to 4096 (Nitin-YK)
4594880  feat: Implement disk and swap architecture for demand paging (Mayur Rajesh Keni)
7769021  Update team member names in README.md
8685669  docs: Update README for Demand Paging project
30b754a  Initial commit: Clean xv6 baseline
``````

## Appendix B: Files Changed (Summary)

```
15 files changed, 293 insertions(+), 56 deletions(-)

 .gitignore |   1 +
 Makefile   |   5 +-
 README.md  |  14 +++++
 defs.h     |   9 +++
 fs.h       |  24 ++++++-
 ide.c      | 210 ++++++++++++++++++++++++++++++++++++++++++++++++++++
 mkfs.c     |   2 +-
 param.h    |   5 ++
 swaptest.c |  17 +++++
 syscall.c  |   2 +
 syscall.h  |   1 +
 sysproc.c  |   6 ++
 user.h     |   1 +
 usys.S     |   1 +
```
