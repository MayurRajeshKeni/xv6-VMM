// memtest: allocate more memory than the capped physical RAM, write a pattern
// to every page, then verify it. Forces the kernel to swap pages out and back.
// usage: memtest [megabytes]   (default 8)
#include "types.h"
#include "stat.h"
#include "user.h"

#define PG 4096

int
main(int argc, char *argv[])
{
  int mb = argc > 1 ? atoi(argv[1]) : 8;
  int pages = mb * 256, i, bad = 0;
  char *base = sbrk(pages * PG);

  if(base == (char*)-1){
    printf(1, "memtest: sbrk failed\n");
    exit();
  }
  printf(1, "memtest: writing %d pages (%d MB)\n", pages, mb);
  for(i = 0; i < pages; i++){
    base[i*PG] = (char)(i & 0x7f);
    base[i*PG + PG-1] = (char)((i*7) & 0x7f);
  }
  printf(1, "memtest: verifying\n");
  for(i = 0; i < pages; i++){
    if(base[i*PG] != (char)(i & 0x7f) || base[i*PG + PG-1] != (char)((i*7) & 0x7f)){
      printf(1, "memtest: MISMATCH on page %d\n", i);
      bad++;
    }
  }
  if(bad)
    printf(1, "memtest: FAILED (%d bad pages)\n", bad);
  else
    printf(1, "memtest: PASSED\n");
  exit();
}
