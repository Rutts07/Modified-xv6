#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

#ifdef LBS
int lottery_scheduler(int total_tickets)
{
  // Take from http://stackoverflow.com/questions/1167253/implementation-of-rand
  static unsigned int z1 = 12345, z2 = 12345, z3 = 12345, z4 = 12345;
  unsigned int b;
  b = ((z1 << 6) ^ z1) >> 13;
  z1 = ((z1 & 4294967294U) << 18) ^ b;
  b = ((z2 << 2) ^ z2) >> 27;
  z2 = ((z2 & 4294967288U) << 2) ^ b;
  b = ((z3 << 13) ^ z3) >> 21;
  z3 = ((z3 & 4294967280U) << 7) ^ b;
  b = ((z4 << 3) ^ z4) >> 12;
  z4 = ((z4 & 4294967168U) << 13) ^ b;

  unsigned int random = (z1 ^ z2 ^ z3 ^ z4) / 2;

  // return a number between 1 and total_tickets
  return (random % total_tickets) + 1;
}
#endif

#ifdef MLFQ
struct Queue queue[MAX_QUEUES];

void enqueue(struct Queue *q, struct proc *p)
{
  if (q->size == NPROC)
    panic("Queue is full");

  q->procs[q->tail] = p;
  q->tail = (q->tail + 1) % (NPROC + 1);
  q->size++;
}

struct proc *top(struct Queue *q)
{
  if (q->head == q->tail)
    return 0;

  return q->procs[q->head];
}

void pop(struct Queue *q)
{
  if (q->size == 0)
    panic("Queue is empty");

  q->head = (q->head + 1) % (NPROC + 1);
  q->size--;
}

void dequeue(struct Queue *q, struct proc *p)
{
  // find the process in the queue
  for (int i = q->head; i != q->tail; i = (i + 1) % (NPROC + 1))
  {
    if (q->procs[i]->pid == p->pid)
    {
      // shift all the processes after the process to be removed
      for (int j = i; j != q->tail; j = (j + 1) % NPROC)
        q->procs[j] = q->procs[(j + 1) % NPROC];

      q->tail = q->tail - 1 < 0 ? NPROC : q->tail - 1;
      q->size--;

      return;
    }
  }
}
#endif

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }

// Initialize the MLFQ
#ifdef MLFQ
  for (int i = 0; i < MAX_QUEUES; i++)
  {
    // queue[i] =
    queue[i].head = 0;
    queue[i].tail = 0;
    queue[i].size = 0;
  }
#endif
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Allocate a backup trapframe page.
  if ((p->trapframe_backup = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  p->curticks = 0;
  p->handler = 0;
  p->alarm = 0;
  p->ticks = 0;

  // initialise ticks for each process for FCFS
  p->c_time = ticks;

#ifdef PBS
  p->priority = 60;
  p->scheduled = 0;
  p->runtime = 0;
  p->waitime = 0;
#endif

#ifdef LBS
  p->tickets = 1;
#endif

// Push the process to the lowest (highest priority) queue
#ifdef MLFQ
  p->priority = 0;
  p->change_queue = 1; // flag 1 because the process is new
  p->rem_ticks = 1 << p->priority;
  p->in_time = ticks;

  for (int i = 0; i < MAX_QUEUES; i++)
  {
    p->q_ticks[i] = 0;
  }

#ifdef YES
  printf("Alloc : %d %d %d\n", p->pid, p->priority, ticks);
#endif
#endif

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
#ifdef YES
#ifdef MLFQ
  printf("Ended : %d %d %d\n", p->pid, p->priority, ticks);
#endif
#endif

  if (p->trapframe)
    kfree((void *)p->trapframe);

  if (p->trapframe_backup)
    kfree((void *)p->trapframe_backup);

  p->trapframe = 0;
  p->trapframe_backup = 0;

  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);

  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;

  p->curticks = 0;
  p->handler = 0;
  p->alarm = 0;
  p->ticks = 0;

  p->c_time = 0;
  p->e_time = 0;

  p->t_runtime = 0;
  p->t_waitime = 0;

#ifdef PBS
  p->priority = 60;
  p->scheduled = 0;
  p->runtime = 0;
  p->waitime = 0;
#endif

#ifdef LBS
  p->tickets = 1;
#endif

#ifdef MLFQ
  p->priority = 0;
  p->change_queue = 0;
  p->rem_ticks = 1;
  p->in_time = 0;
#endif
}

// Update process' runtime and waitime variables for PBS
void update_time(void)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);

    if (p->state == RUNNING)
    {
      p->t_runtime++;

#ifdef PBS
      p->runtime += 1;
#endif

#ifdef MLFQ
      p->q_ticks[p->priority] += 1;
      p->rem_ticks -= 1;

/*
printf("\n %d : ", p->pid);
for (int i = 0; i < MAX_QUEUES; i++)
  printf("%d ", p->q_ticks[i]);
printf("\n");
*/
#endif
    }

    else if (p->state == SLEEPING)
    {
#ifdef PBS
      p->waitime += 1;
#endif
    }

    release(&p->lock);
  }
}

#ifdef PBS
int dynamic_priority(struct proc *p)
{
  int niceness, d_priority;

  // Niceness equal 5 for new processes
  if (p->runtime == 0 && p->waitime == 0)
    niceness = 5;

  else
    niceness = p->waitime * 10 / (p->runtime + p->waitime);

  d_priority = p->priority - niceness + 5;
  d_priority = (d_priority > 100) ? 100 : d_priority;
  d_priority = (d_priority < 0) ? 0 : d_priority;

  return d_priority;
}

int set_priority(int priority, int pid)
{
  struct proc *p;
  int old_priority = -1;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      acquire(&p->lock);
      old_priority = p->priority;
      p->priority = priority;
      p->runtime = 0;
      release(&p->lock);

      // Yield if the process acquires a higher priority
      if (old_priority > priority)
        yield();
    }
  }

  return old_priority;
}
#endif

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // copy trace masks to child processes.
  np->tracemask = p->tracemask;

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

#ifdef LBS
  // Copy parent's tickets to child
  np->tickets = p->tickets;
#endif

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->e_time = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

#ifdef RR
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
#endif

#ifdef FCFS
    struct proc *next = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // choose the process with the earliest creation time
        if (next == 0 || p->c_time < next->c_time)
        {
          if (next != 0)
            release(&next->lock);

          // Don't release the lock of the chosen process, unless a better process is found
          next = p;
          continue;
        }
      }

      release(&p->lock);
    }

    if (next == 0)
      continue;

    // Lock already acquired
    next->state = RUNNING;
    c->proc = next;

    swtch(&c->context, &next->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&next->lock);
#endif

#ifdef LBS
    struct proc *next = 0;

    int total_tickets = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);

      if (p->state == RUNNABLE)
        total_tickets += p->tickets;

      release(&p->lock);
    }

    // Probabilistic lottery
    int winner = lottery_scheduler(total_tickets);

    int counter = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);

      if (p->state == RUNNABLE)
      {
        counter += p->tickets;
        if (next == 0 || counter >= winner)
        {
          if (next == 0)
            next = p;

          else
          {
            release(&next->lock);

            // Don't release the lock of the chosen process
            next = p;
            break;
          }

          continue;
        }
      }

      release(&p->lock);
    }

    if (next == 0)
      continue;

    // Lock already acquired
    next->state = RUNNING;
    c->proc = next;

    swtch(&c->context, &next->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&next->lock);
#endif

#ifdef PBS
    struct proc *next = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // choose the process with the lowest priority
        if (next == 0 || dynamic_priority(p) <= dynamic_priority(next))
        {
          if (next == 0)
          {
            next = p;
            continue;
          }

          // If the priorities are equal
          if (dynamic_priority(p) == dynamic_priority(next))
          {
            // choose the process scheduled less
            if (p->scheduled <= next->scheduled)
            {
              // If the number of times the processes have been scheduled is the same
              if (p->scheduled == next->scheduled)
              {
                // choose the process with the latest creation time
                if (p->c_time > next->c_time)
                {
                  release(&next->lock);
                  next = p;
                  continue;
                }

                // if the creation times are equal or the process with the latest creation time is already chosen
                release(&p->lock);
                continue;
              }

              release(&next->lock);
              next = p;
              continue;
            }

            // if the process with the least number of times scheduled is already chosen
            release(&p->lock);
            continue;
          }

          release(&next->lock);
          next = p;
          continue;
        }
      }

      release(&p->lock);
    }

    if (next == 0)
      continue;

    // Lock already acquired
    next->state = RUNNING;
    next->scheduled = next->scheduled + 1;

    // Reset and Re-calculate runtime and wait time
    next->runtime = 0;
    next->waitime = 0;
    c->proc = next;

    swtch(&c->context, &next->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&next->lock);
#endif

#ifdef MLFQ
    struct proc *next = 0;

    // Check for starvation
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);

      // If the process has been waiting for too long
      if (p->state == RUNNABLE && (ticks - p->in_time >= MAX_SLICES))
      {
        dequeue(&queue[p->priority], p);

        // Promote it to a lower queue and reset entry time
        if (p->priority > 0)
          p->priority = p->priority - 1;

        p->change_queue = 1;
      }

      release(&p->lock);
    }

    // Update the queue for all process that need a queue change
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);

      if (p->state == RUNNABLE && p->change_queue == 1)
      {
#ifdef YES
        printf("Q_Change : %d %d %d\n", p->pid, p->priority, ticks);
#endif

        enqueue(&queue[p->priority], p);
        p->change_queue = 0;
      }

      release(&p->lock);
    }

    // Choose a process from the highest non-empty priority queue
    for (int i = 0; i < MAX_QUEUES; i++)
    {
      while (queue[i].size > 0)
      {
        p = top(&queue[i]);

        if (p == 0)
          continue;

        acquire(&p->lock);

        // Remove the process from the queue, the process can be sleeping also
        dequeue(&queue[i], p);
        p->change_queue = 1;

        if (p->state == RUNNABLE)
        {
          // Don't release the lock of the chosen process
          p->in_time = ticks;
          next = p;
          break;
        }

        release(&p->lock);
      }

      if (next != 0)
        break;
    }

    if (next == 0)
      continue;

    // Lock already acquired
    // Allot the process a time slice (equal to bit shift of its priority)
    next->rem_ticks = 1 << next->priority;
    next->state = RUNNING;

    // Reset and Re-calculate runtime and wait time
    c->proc = next;

    swtch(&c->context, &next->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    next->in_time = ticks;
    release(&next->lock);
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

int waitx(uint64 addr, int *rtime, int *wtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          *rtime = np->t_runtime;
          *wtime = np->e_time - np->c_time - np->t_runtime;

          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;

    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];

    else
      state = "???";

    printf("%d %s %s", p->pid, state, p->name);

#ifdef MLFQ
    printf(" Queue No: %d ", p->priority);
    for (int i = 0; i < MAX_QUEUES; i++)
      printf("ticks[%d]: %d ", i, p->q_ticks[i]);
#endif
    printf("\n");
  }
}
