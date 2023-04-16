#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

int
sys_verify(void)
{
  char *username, *password;
  if(argstr(0, &username) < 0)
    return -1;
  if(argstr(1, &password) < 0)
    return -1;
  return verify(username, password);
}

int
sys_logout(void)
{
  return logout();
}