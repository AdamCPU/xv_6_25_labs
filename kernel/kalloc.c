// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define PPN_CNT ((PHYSTOP - KERNBASE) / PGSIZE)
#define PA2IND(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

char *new_end;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  char *refs;
} kmem;

void*
initref(){
  kmem.refs = end;
  new_end = kmem.refs + PPN_CNT;
  memset(kmem.refs, 1, PPN_CNT);
  return new_end;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(initref(), (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < new_end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  //memset(pa, 1, PGSIZE); // TOTO ODSTRANUJEME

  //r = (struct run*)pa;

  acquire(&kmem.lock);
  kmem.refs[PA2IND(pa)]--;
  if(kmem.refs[PA2IND(pa)] > 0) {
    release(&kmem.lock);
    return;
  } else if (kmem.refs[PA2IND(pa)] < 0){
    panic("kfree: underflow");
  }

  memset(pa, 1, PGSIZE); // TOTO ODSTRANUJEME
  r = (struct run*)pa;
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
  if(r){
    kmem.refs[PA2IND(r)]++;
    if(kmem.refs[PA2IND(r)] != 1){
      panic("kalloc: refcnt != 1");
    }
    kmem.freelist = r->next;
    }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void
getref(void *pa){
  acquire(&kmem.lock);
  kmem.refs[PA2IND(pa)]++;
  if(kmem.refs[PA2IND(pa
)] <= 0){
    panic("kalloc: refcnt <= 0");
  }
  release(&kmem.lock);
}