#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  // set the tracemask to the first argument of the syscall
  argint(0, &myproc()->tracemask);
  return 0;
}

uint64
sys_sigalarm(void)
{
  int ticks;
  uint64 handler;

  argint(0, &ticks);
  argaddr(1, &handler);

  myproc()->alarm = 0;
  myproc()->curticks = 0;
  myproc()->ticks = ticks;
  myproc()->handler = handler;

  // printf("ticks: %d, handler: %p\n", ticks, handler);
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  // resume the process from the signal handler
  memmove(p->trapframe, p->trapframe_backup, PGSIZE);
  kfree(p->trapframe_backup);

  p->trapframe_backup = 0;
  p->curticks = 0;
  p->alarm = 0;

  usertrapret();
  return 0;
}

uint64
sys_set_priority(void)
{
  // Only applicable to Priority Based Scheduler
  #ifndef PBS
    return -1;
  #endif

  int pid, priority;

  argint(0, &priority);
  argint(1, &pid);

  int old_priority = set_priority(pid, priority);
  return old_priority;
}

uint64
sys_settickets(void)
{
  // Only applicable to Lottery Based Scheduler
  #ifdef LBS
  int tickets;

  argint(0, &tickets);

  // A process can only raise its tickets and not lower them
  if (tickets < myproc()->tickets)
    return -1;

  myproc()->tickets = tickets;
  return 0;
  #endif

  return -1;
}

uint64
sys_waitx(void)
{
  uint64 addr, addr1, addr2;
  int wtime, rtime;

  argaddr(0, &addr);
  argaddr(1, &addr1); // user virtual memory
  argaddr(2, &addr2);

  int ret = waitx(addr, &wtime, &rtime);

  struct proc* p = myproc();

  if (copyout(p->pagetable, addr1,(char*)&wtime, sizeof(int)) < 0)
    return -1;

  if (copyout(p->pagetable, addr2,(char*)&rtime, sizeof(int)) < 0)
    return -1;

  return ret;
}
