// forkswap: fill memory beyond the cap, fork, and check parent and child each
// see the right data (exercises copyuvm reading swapped-out parent pages).
#include "types.h"
#include "stat.h"
#include "user.h"
#define PG 4096
int
main(void)
{
  int pages = 1536, i, bad = 0, pid;   // 6 MB
  char *b = sbrk(pages * PG);
  for(i = 0; i < pages; i++) b[i*PG] = (char)(i & 0x7f);
  pid = fork();
  if(pid < 0){ printf(1, "forkswap: fork FAILED\n"); exit(); }
  if(pid == 0){
    for(i = 0; i < pages; i++){ if(b[i*PG] != (char)(i & 0x7f)) bad++; b[i*PG] = 0x55; }
    printf(1, "forkswap child: %s\n", bad ? "FAILED" : "PASSED");
    exit();
  }
  wait();
  for(i = 0; i < pages; i++) if(b[i*PG] != (char)(i & 0x7f)) bad++;
  printf(1, "forkswap parent: %s\n", bad ? "FAILED" : "PASSED");
  exit();
}
