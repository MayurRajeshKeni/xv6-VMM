#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "swap.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
// ---- Adapter: Nitin's eviction code <-> Mayur's swap API (ide.c) -----------
// swapout: reserve a slot and write the page there. Returns slot or -1.
static int
swapout(char *kva)
{
  int slot = swap_alloc();
  if(slot < 0)
    return -1;
  if(swap_write((uint*)kva, slot) < 0){
    swap_free(slot);
    return -1;
  }
  return slot;
}
// swapin: read slot into kva (slot stays allocated until swapfree).
static void
swapin(uint slot, char *kva)
{
  if(swap_read((uint*)kva, slot) < 0)
    panic("swapin: read failed");
}
static void
swapfree(uint slot)
{
  swap_free(slot);
}

// ---------------------------------------------------------------------------
// Demand paging: frame registry + clock (second-chance) eviction  [Nitin]
//
// frames[] is a reverse map: for every physical frame that holds a user page
// it records which page table and virtual address map it. The evictor walks
// this array like the hand of a clock, using the PTE_A (accessed) bit that the
// CPU sets on every access to give recently used pages a second chance.
//
// evictlk serialises everything that changes which pages are resident
// (eviction, page-in, freeing user memory, fork's copy). It is a sleeplock
// because eviction does disk I/O while holding it.
// ---------------------------------------------------------------------------
#define NFRAMES (PHYSCAP / PGSIZE)

static struct frame {
  pde_t *pgdir;     // owner page table; 0 if frame is not a user page
  uint va;          // virtual address in that page table
  uint pinned;      // kernel is using this page across a sleep; don't evict
} frames[NFRAMES];

static struct sleeplock evictlk;
static uint clockhand;

// Boot-time callers (userinit) run before any process exists, where sleeplocks
// can't be used; there is no concurrency yet so the lock is skipped.
static void evlock(void)   { if(myproc()) acquiresleep(&evictlk); }
static void evunlock(void) { if(myproc()) releasesleep(&evictlk); }

static void
frame_set(uint pa, pde_t *pgdir, uint va)
{
  if(pa / PGSIZE >= NFRAMES)
    panic("frame_set: frame above PHYSCAP");
  frames[pa / PGSIZE].pgdir = pgdir;
  frames[pa / PGSIZE].va = va;
  frames[pa / PGSIZE].pinned = 0;
}

static void
frame_clear(uint pa)
{
  if(pa / PGSIZE >= NFRAMES)
    panic("frame_clear: frame above PHYSCAP");
  frames[pa / PGSIZE].pgdir = 0;
}

// Pick a victim with the clock algorithm, write it to swap, mark its PTE as
// swapped and free the frame. Caller holds evictlk. Returns 1 if a frame was
// freed, 0 if nothing could be evicted.
static int
evict_locked(void)
{
  struct frame *f;
  pte_t *pte;
  uint i, idx, pa, flags;
  int slot;

  for(i = 0; i < 2*NFRAMES; i++){        // 2 sweeps: first clears A bits
    idx = clockhand;
    clockhand = (clockhand + 1) % NFRAMES;
    f = &frames[idx];
    if(f->pgdir == 0 || f->pinned)
      continue;
    pte = walkpgdir(f->pgdir, (void*)f->va, 0);
    if(pte == 0 || !(*pte & PTE_P))
      panic("evict: frame registry out of sync");
    if(*pte & PTE_A){                    // recently used: second chance
      *pte &= ~PTE_A;
      continue;
    }

    // Victim found.
    pa = idx * PGSIZE;
    flags = *pte & (PTE_W | PTE_U);
    *pte &= ~PTE_P;                      // owner now faults instead of writing
    lcr3(V2P(myproc()->pgdir));          // flush this CPU's TLB

    slot = swapout(P2V(pa));
    if(slot < 0){                        // swap full
      *pte |= PTE_P;
      return 0;
    }
    *pte = (slot << PTXSHIFT) | flags | PTE_SWAPPED;
    f->pgdir = 0;
#if SWAP_VERBOSE
    cprintf("[swap] evict va=0x%x pa=0x%x -> slot %d\n", f->va, pa, slot);
#endif
    kfree(P2V(pa));
    return 1;
  }
  return 0;
}

// Called by kalloc() when the free list is empty. Returns 1 if a page was
// freed (caller should retry), 0 if no memory could be reclaimed.
int
evict_page(void)
{
  int r, nospin;

  if(myproc() == 0)                      // boot: can't sleep, nothing to evict
    return 0;
  pushcli();
  nospin = (mycpu()->ncli == 1);         // only our own pushcli => no spinlocks held
  popcli();
  if(!nospin)                            // holding a spinlock: sleeping would panic
    return 0;
  if(holdingsleep(&evictlk))             // re-entered from inside the evictor
    return 0;

  acquiresleep(&evictlk);
  r = evict_locked();
  releasesleep(&evictlk);
  return r;
}

// Bring the swapped-out page containing user address va back into memory.
// Called from the T_PGFLT handler (Hari) and from kernel paths that touch user
// memory. Returns 0 on success, -1 if va is not a swapped page of this process.
int
swapinpage(pde_t *pgdir, uint va)
{
  char *mem;
  pte_t *pte;
  uint slot, flags;

  va = PGROUNDDOWN(va);
  if(va >= KERNBASE)
    return -1;
  mem = kalloc();                        // may itself evict another page
  if(mem == 0)
    return -1;

  evlock();
  pte = walkpgdir(pgdir, (void*)va, 0);
  if(pte && (*pte & PTE_P)){             // someone else already paged it in
    evunlock();
    kfree(mem);
    return 0;
  }
  if(pte == 0 || !(*pte & PTE_SWAPPED)){
    evunlock();
    kfree(mem);
    return -1;
  }
  slot = PTE_SLOT(*pte);
  flags = PTE_P | (*pte & (PTE_W | PTE_U));
  swapin(slot, mem);
  swapfree(slot);
  *pte = V2P(mem) | flags;
  frame_set(V2P(mem), pgdir, va);
#if SWAP_VERBOSE
  cprintf("[swap] swapin va=0x%x <- slot %d\n", va, slot);
#endif
  evunlock();
  return 0;
}

void
kvmalloc(void)
{
  initsleeplock(&evictlk, "evict");
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  evlock();
  frame_set(V2P(mem), pgdir, 0);
  evunlock();
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n, va;
  pte_t *pte;
  int r;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    va = (uint)addr + i;
    // Make the page resident and pin it: readi() sleeps on disk I/O, and the
    // frame must not be evicted/reused underneath it.
    for(;;){
      evlock();
      if((pte = walkpgdir(pgdir, (void*)va, 0)) == 0)
        panic("loaduvm: address should exist");
      if(*pte & PTE_P){
        pa = PTE_ADDR(*pte);
        frames[pa / PGSIZE].pinned = 1;
        evunlock();
        break;
      }
      evunlock();
      if(swapinpage(pgdir, va) < 0)
        panic("loaduvm: cannot swap page in");
    }
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    r = readi(ip, P2V(pa), offset+i, n);
    evlock();
    frames[pa / PGSIZE].pinned = 0;
    evunlock();
    if(r != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
    evlock();
    frame_set(V2P(mem), pgdir, a);
    evunlock();
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;
  int own;

  if(newsz >= oldsz)
    return oldsz;

  // Hold evictlk so the evictor can't be mid-way through one of our pages.
  own = (myproc() != 0 && !holdingsleep(&evictlk));
  if(own)
    acquiresleep(&evictlk);

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      frame_clear(pa);
      kfree(v);
      *pte = 0;
    } else if(*pte & PTE_SWAPPED){       // page lives on disk: release its slot
      swapfree(PTE_SLOT(*pte));
      *pte = 0;
    }
  }

  if(own)
    releasesleep(&evictlk);
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    // Allocate everything that can trigger eviction (data page, and the child's
    // page-table page) before taking evictlk.
    if((mem = kalloc()) == 0)
      goto bad;
    if(walkpgdir(d, (void*)i, 1) == 0){
      kfree(mem);
      goto bad;
    }
    evlock();
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(*pte & PTE_P){
      pa = PTE_ADDR(*pte);
      flags = PTE_FLAGS(*pte);
      memmove(mem, (char*)P2V(pa), PGSIZE);
    } else if(*pte & PTE_SWAPPED){       // parent's page is on disk: read it
      flags = PTE_P | (*pte & (PTE_W | PTE_U));   // straight into the child
      swapin(PTE_SLOT(*pte), mem);
    } else
      panic("copyuvm: page not present");
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      evunlock();
      kfree(mem);
      goto bad;
    }
    frame_set(V2P(mem), d, i);
    evunlock();
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0 || (*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf;
  pte_t *pte;
  int swapped;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    for(;;){
      evlock();                          // keep the page resident while we write
      pte = walkpgdir(pgdir, (char*)va0, 0);
      if(pte && (*pte & PTE_P)){
        if((*pte & PTE_U) == 0){         // e.g. stack guard page: not ours to write
          evunlock();
          return -1;
        }
        memmove(P2V(PTE_ADDR(*pte)) + (va - va0), buf, n);
        evunlock();
        break;
      }
      swapped = pte && (*pte & PTE_SWAPPED);
      evunlock();
      if(!swapped || swapinpage(pgdir, va0) < 0)   // swapped out? bring it back, retry
        return -1;
    }
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

