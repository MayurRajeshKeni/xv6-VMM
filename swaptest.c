#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  printf(1, "Running swap subsystem test...\n");
  int res = swaptest();
  if(res == 0){
    printf(1, "TEST PASSED: Disk swap read/write verified successfully!\n");
  } else {
    printf(1, "TEST FAILED: Disk swap test failed with return code %d\n", res);
  }
  exit();
}

