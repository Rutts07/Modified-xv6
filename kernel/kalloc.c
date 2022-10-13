// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

/////////////////////
// Structure to keep track of number of references to a page.
struct refcnt{
  struct spinlock lock;
  uint counter[(PHYSTOP - KERNBASE) / PGSIZE];
};

struct refcnt pagerefs;

inline
uint64
pgindex(uint64 pa){
  return (pa - KERNBASE) / PGSIZE;
}

inline
void
acquire_refcnt(a){
  acquire(&pagerefs.lock);
}

inline
void
release_refcnt(a){
  release(&pagerefs.lock);
}

void
refcnt_setter(uint64 pa, int n){
  pagerefs.counter[pgindex((uint64)pa)] = n;
}

inline
uint
refcnt_getter(uint64 pa){
  return pagerefs.counter[pgindex(pa)];
}

void
refcnt_incr(uint64 pa, int n){
  acquire(&pagerefs.lock);
  pagerefs.counter[pgindex(pa)] += n;
  release(&pagerefs.lock);
}
//////////////////////

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

void
kfree(void *pa)
{
  struct run *r;

  // page with refcnt  1 should not be freed
  // acquire_refcnt();
  acquire(&pagerefs.lock);
  if(pagerefs.counter[pgindex((uint64)pa)] > 1){
    pagerefs.counter[pgindex((uint64)pa)] -= 1;
    release(&pagerefs.lock);
    return;
  }

  // if(((uint64)pa % PGSIZE) != 0| | -char*)pa  end || (uint64)pa = PHYSTOP)
  //   panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  pagerefs.counter[pgindex((uint64)pa)] = 0;
  release(&pagerefs.lock);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}


// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  if (r)
  {
    acquire(&pagerefs.lock);
    pagerefs.counter[pgindex((uint64)r)] += 1;
    release(&pagerefs.lock);
  }
    
  return (void*)r;
}


void *
kalloc_initialise(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  if (r)
  {
    acquire(&pagerefs.lock);
    pagerefs.counter[pgindex((uint64)r)] = 1;
    release(&pagerefs.lock);
  }
    
  return (void*)r;
}
