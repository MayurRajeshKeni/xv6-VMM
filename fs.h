#ifndef _FS_H_
#define _FS_H_

#include "param.h"

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Swap boundaries and configuration
#ifndef PGSIZE
#define PGSIZE 4096 // 4KB page size
#endif
#define SECTORS_PER_PAGE  (PGSIZE / BSIZE)  // 8 sectors per 4KB memory page
#define SWAP_START        (FSSIZE)          // exact disk sector offset where swap begins (right after FS data)
#define SWAP_SECTORS      (MAX_SWAP_PAGES * SECTORS_PER_PAGE) // total disk sectors reserved for swap
#define TOTAL_DISK_BLOCKS (FSSIZE + SWAP_SECTORS)             // total disk blocks including file system & swap
#define SWAP_SLOT_TO_SECTOR(slot) (SWAP_START + ((slot) * SECTORS_PER_PAGE))

// Swap slot entry structure for tracking allocation state
struct swap_entry {
  int is_free;          // 1 if slot is free, 0 if allocated
  uint page_perm;       // page flags/permissions (optional metadata)
};

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

#endif

