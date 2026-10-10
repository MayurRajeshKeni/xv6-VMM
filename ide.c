// Simple PIO-based (non-DMA) IDE driver code.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#define SECTOR_SIZE   512
#define IDE_BSY       0x80
#define IDE_DRDY      0x40
#define IDE_DF        0x20
#define IDE_ERR       0x01

#define IDE_CMD_READ  0x20
#define IDE_CMD_WRITE 0x30
#define IDE_CMD_RDMUL 0xc4
#define IDE_CMD_WRMUL 0xc5

// idequeue points to the buf now being read/written to the disk.
// idequeue->qnext points to the next buf to be processed.
// You must hold idelock while manipulating queue.

static struct spinlock idelock;
static struct buf *idequeue;

static int havedisk1;
static void idestart(struct buf*);

// Wait for IDE disk to become ready.
static int
idewait(int checkerr)
{
  int r;

  while(((r = inb(0x1f7)) & (IDE_BSY|IDE_DRDY)) != IDE_DRDY)
    ;
  if(checkerr && (r & (IDE_DF|IDE_ERR)) != 0)
    return -1;
  return 0;
}

void
ideinit(void)
{
  int i;

  initlock(&idelock, "ide");
  ioapicenable(IRQ_IDE, ncpu - 1);
  idewait(0);

  // Check if disk 1 is present
  outb(0x1f6, 0xe0 | (1<<4));
  for(i=0; i<1000; i++){
    if(inb(0x1f7) != 0){
      havedisk1 = 1;
      break;
    }
  }

  // Switch back to disk 0.
  outb(0x1f6, 0xe0 | (0<<4));

  // Initialize the swap subsystem
  swap_init();
}

// Start the request for b.  Caller must hold idelock.
static void
idestart(struct buf *b)
{
  if(b == 0)
    panic("idestart");
  if(b->blockno >= TOTAL_DISK_BLOCKS)
    panic("incorrect blockno");
  int sector_per_block =  BSIZE/SECTOR_SIZE;
  int sector = b->blockno * sector_per_block;
  int read_cmd = (sector_per_block == 1) ? IDE_CMD_READ :  IDE_CMD_RDMUL;
  int write_cmd = (sector_per_block == 1) ? IDE_CMD_WRITE : IDE_CMD_WRMUL;

  if (sector_per_block > 7) panic("idestart");

  idewait(0);
  outb(0x3f6, 0);  // generate interrupt
  outb(0x1f2, sector_per_block);  // number of sectors
  outb(0x1f3, sector & 0xff);
  outb(0x1f4, (sector >> 8) & 0xff);
  outb(0x1f5, (sector >> 16) & 0xff);
  outb(0x1f6, 0xe0 | ((b->dev&1)<<4) | ((sector>>24)&0x0f));
  if(b->flags & B_DIRTY){
    outb(0x1f7, write_cmd);
    outsl(0x1f0, b->data, BSIZE/4);
  } else {
    outb(0x1f7, read_cmd);
  }
}

// Interrupt handler.
void
ideintr(void)
{
  struct buf *b;

  // First queued buffer is the active request.
  acquire(&idelock);

  if((b = idequeue) == 0){
    release(&idelock);
    return;
  }
  idequeue = b->qnext;

  // Read data if needed.
  if(!(b->flags & B_DIRTY) && idewait(1) >= 0)
    insl(0x1f0, b->data, BSIZE/4);

  // Wake process waiting for this buf.
  b->flags |= B_VALID;
  b->flags &= ~B_DIRTY;
  wakeup(b);

  // Start disk on next buf in queue.
  if(idequeue != 0)
    idestart(idequeue);

  release(&idelock);
}

//PAGEBREAK!
// Sync buf with disk.
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
void
iderw(struct buf *b)
{
  struct buf **pp;

  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(b->dev != 0 && !havedisk1)
    panic("iderw: ide disk 1 not present");

  acquire(&idelock);  //DOC:acquire-lock

  // Append b to idequeue.
  b->qnext = 0;
  for(pp=&idequeue; *pp; pp=&(*pp)->qnext)  //DOC:insert-queue
    ;
  *pp = b;

  // Start disk if necessary.
  if(idequeue == b)
    idestart(b);

  // Wait for request to finish.
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    sleep(b, &idelock);
  }


  release(&idelock);
}

//
// =====================================================================
// Swap Subsystem (Disk & Swap Architecture)
// =====================================================================
//

static struct {
  struct spinlock lock;
  struct swap_entry entries[MAX_SWAP_PAGES];
} swap_table;

// Initialize swap table and lock
void
swap_init(void)
{
  int i;

  initlock(&swap_table.lock, "swap_table");
  acquire(&swap_table.lock);
  for(i = 0; i < MAX_SWAP_PAGES; i++){
    swap_table.entries[i].is_free = 1;
    swap_table.entries[i].page_perm = 0;
  }
  release(&swap_table.lock);
}

// Allocate a free swap slot; returns slot index (0 .. MAX_SWAP_PAGES-1) or -1 if full
int
swap_alloc(void)
{
  int i;

  acquire(&swap_table.lock);
  for(i = 0; i < MAX_SWAP_PAGES; i++){
    if(swap_table.entries[i].is_free){
      swap_table.entries[i].is_free = 0;
      release(&swap_table.lock);
      return i;
    }
  }
  release(&swap_table.lock);
  return -1;
}

// Free an allocated swap slot
void
swap_free(uint swap_slot)
{
  if(swap_slot >= MAX_SWAP_PAGES)
    panic("swap_free: slot out of range");

  acquire(&swap_table.lock);
  swap_table.entries[swap_slot].is_free = 1;
  swap_table.entries[swap_slot].page_perm = 0;
  release(&swap_table.lock);
}

// Check if a swap slot is free (1 = free, 0 = allocated)
int
swap_is_free(uint swap_slot)
{
  if(swap_slot >= MAX_SWAP_PAGES)
    return 0;

  acquire(&swap_table.lock);
  int free = swap_table.entries[swap_slot].is_free;
  release(&swap_table.lock);
  return free;
}

// Set page permission metadata for a swap slot
void
swap_set_perm(uint swap_slot, uint perm)
{
  if(swap_slot >= MAX_SWAP_PAGES)
    return;

  acquire(&swap_table.lock);
  swap_table.entries[swap_slot].page_perm = perm;
  release(&swap_table.lock);
}

// Get page permission metadata for a swap slot
uint
swap_get_perm(uint swap_slot)
{
  if(swap_slot >= MAX_SWAP_PAGES)
    return 0;

  acquire(&swap_table.lock);
  uint perm = swap_table.entries[swap_slot].page_perm;
  release(&swap_table.lock);
  return perm;
}

// Write a 4KB memory page across 8 consecutive 512-byte disk sectors in the swap area
int
swap_write(uint *page_virtual_address, uint swap_slot)
{
  if(swap_slot >= MAX_SWAP_PAGES || page_virtual_address == 0)
    return -1;

  char *addr = (char*)page_virtual_address;
  uint start_sector = SWAP_SLOT_TO_SECTOR(swap_slot);
  struct buf b;
  int i;

  memset(&b, 0, sizeof(b));
  initsleeplock(&b.lock, "swapbuf");
  acquiresleep(&b.lock);
  b.dev = ROOTDEV;

  for(i = 0; i < SECTORS_PER_PAGE; i++){
    b.blockno = start_sector + i;
    b.flags = B_DIRTY;
    memmove(b.data, addr + (i * BSIZE), BSIZE);
    iderw(&b);
  }

  releasesleep(&b.lock);
  return 0;
}

// Read 8 consecutive 512-byte disk sectors from a swap slot back into a 4KB physical page
int
swap_read(uint *page_virtual_address, uint swap_slot)
{
  if(swap_slot >= MAX_SWAP_PAGES || page_virtual_address == 0)
    return -1;

  char *addr = (char*)page_virtual_address;
  uint start_sector = SWAP_SLOT_TO_SECTOR(swap_slot);
  struct buf b;
  int i;

  memset(&b, 0, sizeof(b));
  initsleeplock(&b.lock, "swapbuf");
  acquiresleep(&b.lock);
  b.dev = ROOTDEV;

  for(i = 0; i < SECTORS_PER_PAGE; i++){
    b.blockno = start_sector + i;
    b.flags = 0;
    iderw(&b);
    memmove(addr + (i * BSIZE), b.data, BSIZE);
  }

  releasesleep(&b.lock);
  return 0;
}

// Self-test verifying disk swap read/write functionality
int
swap_test(void)
{
  int slot = swap_alloc();
  if(slot < 0){
    cprintf("swap_test: FAILED to allocate swap slot\n");
    return -1;
  }

  char *page = kalloc();
  if(!page){
    cprintf("swap_test: FAILED to allocate memory page\n");
    swap_free(slot);
    return -1;
  }

  int i;
  for(i = 0; i < PGSIZE; i++){
    page[i] = (char)(i % 251 + 1);
  }

  if(swap_write((uint*)page, slot) < 0){
    cprintf("swap_test: FAILED swap_write\n");
    kfree(page);
    swap_free(slot);
    return -1;
  }

  memset(page, 0, PGSIZE);

  if(swap_read((uint*)page, slot) < 0){
    cprintf("swap_test: FAILED swap_read\n");
    kfree(page);
    swap_free(slot);
    return -1;
  }

  for(i = 0; i < PGSIZE; i++){
    if(page[i] != (char)(i % 251 + 1)){
      cprintf("swap_test: FAILED data mismatch at byte %d (expected %d, got %d)\n",
              i, (char)(i % 251 + 1), page[i]);
      kfree(page);
      swap_free(slot);
      return -1;
    }
  }

  kfree(page);
  swap_free(slot);
  cprintf("swap_test: PASSED! 4KB page write/read verified across 8 disk sectors.\n");
  return 0;
}
