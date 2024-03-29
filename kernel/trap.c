#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

#ifdef MLFQ
extern struct Queue queue[MAX_QUEUES];
#endif

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void usertrap(void)
{
  int which_dev = 0;

  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  if (r_scause() == 8)
  {
    // system call

    if (killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  }

  else if (r_scause() == 15) 
  {
    // Modify usertrap() to recognize page faults.
    // When a page-fault occurs on a COW page, allocate a new page with kalloc(),
    // copy the old page to the new page,
    // and install the new page in the PTE with PTE_W set.

    // Get physial page address and correct flags.
    uint64 va = r_stval();

    if(copyOnWrite(va) == - 1)
      p->killed = 1; 
  }

  else if ((which_dev = devintr()) != 0)
  {
    // ok
  }

  else
  {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if (killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    p->curticks += 1;

    if (p->ticks != 0 && p->curticks == p->ticks && p->alarm == 0)
    {
      p->alarm = 1;
      // printf("%d\n", p->trapframe->a0);
      p->curticks = 0;

      // take a backup of the trapframe
      p->trapframe_backup = (struct trapframe *)kalloc();
      memmove(p->trapframe_backup, p->trapframe, PGSIZE);

      // transfer control to the user sigalarm handler
      p->trapframe->epc = p->handler;
    }
    
    // Only pre-empt for RR & LBS Scheduler
    #ifdef RR
      yield();
    #endif

    #ifdef LBS
      yield();
    #endif
  }

  #ifdef MLFQ
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
  {
    // check if the process has exceeded its time quanta of the current queue
    if (myproc()->rem_ticks <= 0)
    {
      myproc()->change_queue = 1;

      if (myproc()->priority < MAX_QUEUES - 1)
        myproc()->priority += 1;
      
      myproc()->rem_ticks = (1 << myproc()->priority);
      yield();
    }

    // check if the higher priority queues have any processes
    // if priority == 0, no preemption until time quanta -> hence round robin for lowest queue
    for (int i = 0; i < myproc()->priority; i++)
    {
      if (queue[i].size != 0)
      {
        yield();
        break;
      }
    }
  }
  #endif

  usertrapret();
}

//
// return to user space
//
void usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0)
  {
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
  {
    #ifdef RR
    yield();
    #endif

    #ifdef LBS
    yield();
    #endif

    #ifdef MLFQ
    if (myproc()->rem_ticks <= 0)
    {
      myproc()->change_queue = 1;

      if (myproc()->priority < MAX_QUEUES - 1)
      {
        myproc()->priority += 1;
        // myproc()->rem_ticks = (1 << myproc()->priority);
      }

      yield();
    }

    // check if the higher priority queues have any processes
    for (int i = 0; i < myproc()->priority; i++)
    {
      if (queue[i].size != 0)
      {
        yield();
        break;
      }
    }
    #endif
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void clockintr()
{
  acquire(&tickslock);
  ticks++;

  update_time();
  
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr()
{
  uint64 scause = r_scause();

  if ((scause & 0x8000000000000000L) &&
      (scause & 0xff) == 9)
  {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if (irq == UART0_IRQ)
    {
      uartintr();
    }
    else if (irq == VIRTIO0_IRQ)
    {
      virtio_disk_intr();
    }
    else if (irq)
    {
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if (irq)
      plic_complete(irq);

    return 1;
  }
  else if (scause == 0x8000000000000001L)
  {
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if (cpuid() == 0)
    {
      clockintr();
    }

    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  }
  else
  {
    return 0;
  }
}


int
copyOnWrite(uint64 va)
{
  va = PGROUNDDOWN(va);
  pagetable_t p = myproc()->pagetable;
  pte_t* pte = walk(p, va, 0);
  uint64 pa = PTE2PA(*pte);
  uint flags = PTE_FLAGS(*pte);

  if(!(flags & PTE_COW))
  {
    panic("Not Cow\n");
    return -10; // not cow page
  }

  acquire_pagerefLock();
  uint ref = pageref_getter(pa);

  // This means that there are multiple processes using this page
  // Hence we need to make a copy.
  if(ref > 1) 
  {
    // create new page
    char* mem = kalloc_initialise();
    if(mem == 0)
    {
      release_pagerefLock();
      return -1;
    };

    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(p, va, PGSIZE, (uint64)mem, (flags & (~PTE_COW)) | PTE_W) !=0)
    {
      kfree(mem);
      release_pagerefLock();
      return -1;
    }
    pageref_setter(pa, ref - 1);
  }
  // This means that there is only one process using this page so we can directly alter  
  else{
    // removing it as a cow page and making it a writable page
    *pte = ((*pte) & (~PTE_COW)) | PTE_W;
  }
  release_pagerefLock();
  return 0;
}
