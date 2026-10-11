// swap.h -- shared contract between the three modules of xv6-VMM.
//
//   Mayur  (disk/swap)   implements swapout / swapin / swapfree
//   Hari   (page fault)  calls swapinpage() from the T_PGFLT handler
//   Nitin  (eviction)    implements swapinpage(), the clock evictor, memory cap

// ---- Memory cap (Nitin) ---------------------------------------------------
// Physical RAM that kalloc() is allowed to hand out. Anything above this is
// never put on the free list, so a process that needs more than ~this much
// memory is forced to swap. Set to PHYSTOP to disable the cap.
// (kinit1 already frees everything between the kernel image and 4MB.)
#define PHYSCAP        (6*1024*1024)

// Set to 1 to print one line per eviction / page-in (useful for the demo).
#define SWAP_VERBOSE   0

// ---- PTE encoding for a swapped-out page (Nitin) --------------------------
// When a page is evicted:   PTE_P = 0
//                           PTE_SWAPPED = 1
//                           bits 31..12 = swap slot number (NOT a physical addr)
//                           PTE_W / PTE_U preserved from the original mapping
#ifndef PTE_A
#define PTE_A          0x020          // accessed bit (set by the CPU), used by clock
#endif
#define PTE_SWAPPED    0x200          // software-available bit 9 in x86 PTEs
#define PTE_SLOT(pte)  ((uint)(pte) >> 12)

// ---- Mayur's disk swap API (ide.c) ----------------------------------------
// Used through the small adapter at the top of vm.c. Declared here too (same
// prototypes as his defs.h, so the duplicate declaration is harmless).
//   int  swap_alloc(void);                  slot (0..MAX_SWAP_PAGES-1) or -1 if full
//   int  swap_write(uint *kva, uint slot);  page at kernel vaddr -> slot; 0 / -1
//   int  swap_read(uint *kva, uint slot);   slot -> page at kernel vaddr; 0 / -1
//   void swap_free(uint slot);
// All may sleep (disk I/O). Slot numbers must fit in 20 bits (PTE bits 31..12).
int  swap_alloc(void);
int  swap_write(uint*, uint);
int  swap_read(uint*, uint);
void swap_free(uint);

// ---- API Nitin provides (used by Hari's trap handler) ---------------------
// int swapinpage(pde_t *pgdir, uint va);
//   Bring the page containing user virtual address va back into RAM.
//   Returns 0 on success, -1 if va is not a valid page of this process
//   (the trap handler should then kill the process as xv6 normally does).
//   Hari's handler: for T_PGFLT where the error code says "page not present"
//   (!(tf->err & 1)), the address is a user address (rcr2() < KERNBASE) and
//   there is a current process, call swapinpage(myproc()->pgdir, rcr2()); if it
//   returns 0, simply break out of the trap switch so the instruction retries.
//   IMPORTANT: do NOT call it for protection faults (tf->err & 1), e.g. the
//   stack guard page -- those are present-but-forbidden and retrying would
//   loop forever. Let them fall through to the normal "kill process" path.
