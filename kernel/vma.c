#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "vma.h"

struct {
  struct spinlock lock;
  struct vma vmas[NVMA];
  struct vma *freelist;
} vma;

// Initialize VMA subsystem.
void
vmainit(void)
{
  // Init VMA subsystem lock.
  initlock(&vma.lock, "vma");

  // Create list of free VMA structs.
  for(struct vma *v = vma.vmas; v < vma.vmas+NVMA; v++){
    v->next = vma.freelist;
    vma.freelist = v;
  }
}

// Allocate a new VMA struct from the global system VMA list.
// Initialize the length `len' of the VMA, duplicate the filedescriptor
// `fd' and set the base address of the new VMA based on the hint `addr'.
struct vma*
vmaalloc(struct vma *head, uint64 addr, uint64 len, struct file *fd)
{
  struct vma *v;

  // Remove VMA from the freelist.
  acquire(&vma.lock);
  v = vma.freelist;
  if(!v)
    panic("vmalloc: no vmas");
  vma.freelist = v->next;
  release(&vma.lock);

  // Translate zero `addr' to the base of the process next VMA, stored in
  // the `head->addr' field.
  if(!addr)
    addr = head->addr;
  // Check for an address hint. If `addr' is not 0, is above VMASTART and
  // interval <addr; addr+len) is not colliding with any existing process
  // VMA, use it. Otherwise, use the address stored in head of process VMA
  // list.
  for(struct vma *v = head->next; addr != head->addr && v != head; v = v->next){
    if((v->addr <= addr && v->addr + v->len > addr) ||
        (v->addr < addr + len && v->addr + len >= addr + len))
      addr = head->addr;
  }

  // Initialize VMA: len, fd and addr.
  v->len = len;
  v->fd = filedup(fd);
  v->addr = addr;

  // Update process VMA head:
  // 1) Update the base of the next process VMA.
  // 2) Update the count of process VMAs.
  if(head->addr < v->addr)
    head->addr = v->addr;
  head->addr += PGROUNDUP(len);
  head->len++;

  // Add VMA to the process VMA list.
  v->next = head->next;
  head->next = v;

  return v;
}

// Add VMA `v' to the system VMAs list, close the file used by this VMA.
void
vmafree(struct vma *v)
{
  struct proc *p = myproc();
  struct vma *vv;

  // Remove VMA from process VMA list.
  for(vv = &p->vma; vv->next != v; vv = vv->next){
    if(vv->next == &p->vma)
      panic("vmfree");
  }
  vv->next = v->next;

  // Close the backend file, first sanity check.
  if(!v->fd)
    panic("vmafree: VMA not backend by a file");
  fileclose(v->fd);

  // Add VMA to the free VMA list.
  acquire(&vma.lock);
  v->next = vma.freelist;
  vma.freelist = v;
  release(&vma.lock);

  // Update count of process VMAs.
  p->vma.len--;
}

// Find the VMA based on the virtual address `addr'. The list of VMAs
// whose head is stored in `head' is searched.
// Return 0 if the VMA is not found, address of the corresponding VMA
// struct otherwise.
struct vma *
vmafind(struct vma *head, uint64 addr)
{
  for(struct vma *v = head->next; v != head; v = v->next){
    if(v->addr <= addr && addr < v->addr+v->len){
      return v;
    }
  }
  return 0;
}

// Copy all VMAs of the parent process `p' stored in head `pvma' into list
// `chvma' of the child process `ch'.
// Return -1 on error, 0 otherwise.
int vmacopy(pagetable_t p, struct vma *pvma, pagetable_t ch, struct vma *chvma)
{
  struct vma *vv;
  char *mem;

  // For each VMA in the parent.
  for(struct vma *v = pvma->next; v != pvma; v = v->next){
    // Allocate a new VMA and fill it; use address hint `addr'.
    vv = vmaalloc(chvma, v->addr, v->len, v->fd);
    vv->prot = v->prot;
    vv->flags = v->flags;
    vv->offset = v->offset;

    // Map and copy mapped pages in the parent's VMA.
    for(uint64 a = vv->addr; a < vv->addr + vv->len; a += PGSIZE){
      // Skip not mapped pages.
      pte_t *pte = walk(p, a, 0);
      if(!pte || !(*pte & PTE_V))
        continue;

      // Allocate memory, copy the data from the parent into the child and map
      // the page into child's VAP.
      if((mem = kalloc()) == 0)
        goto vmacopy_bad;
      memmove(mem, (void*)PTE2PA(*pte), PGSIZE);
      if (mappages(ch, a, PGSIZE, (uint64)mem, PTE_FLAGS(*pte)) != 0)
        goto vmacopy_bad;
    }
  }

  return 0;

vmacopy_bad:
  // Release resources in case of failure.
  if(mem)
    kfree(mem);
  vmaunmap(vv, vv->addr, vv->len);
  return -1;
}

// Unmap a memory region of VMA `v' starting at `addr' of length `len'.
// It is not an error if the region does not contain mapped page(s).
// Modifications to region behind the file length shouldn't be written
// back to the file.
// You can assume that the region will either unmap at the start, or at the
// end, or the whole region (but not punch a hole in the middle of the
// region).
void
vmaunmap(struct vma *v, uint64 addr, uint64 len)
{
  uint64 real_len = len;

  // If `len' go behind the file length, correct it.
  if((addr - v->addr + v->offset) + real_len > v->fd->ip->size)
    real_len = v->fd->ip->size - (addr - v->addr + v->offset);
  // If `addr' itself is behind the file length, the result `real_len' will
  // be a negative number. But, as `real_len' is an unsigned long, the value
  // of `real_len' will be actually greater than `len'. So, correct it.
  if(real_len > len)
    real_len = 0;

  // Walk the indicated range, if any.
  for(uint64 i = 0; i < real_len; i += PGSIZE){
    // Skip non-mapped pages.
    pte_t *pte = walk(myproc()->pagetable, addr + i, 0);
    if(!pte)
      continue;

    // Write back changes to the file.
    if((v->flags & MAP_SHARED) && (v->prot & PROT_WRITE) && (PTE_FLAGS(*pte) & PTE_D)){
      // A file is mapped in multiples of PGSIZE. For a file that is not a
      // multiple of the page size, the remaining bytes in the partial page
      // at the end of the mapping are zeroed when mapped. Modifications to
      // that region are not written out to the file.
      int n = real_len - i > PGSIZE ? PGSIZE : real_len - i;

      // Write changes to the file.
      begin_op();
      ilock(v->fd->ip);
      writei(v->fd->ip, 1, addr + i, v->offset + (addr - v->addr) + i, n);
      iunlock(v->fd->ip);
      end_op();
    }

    // Unmap the corresponding page and free it.
    uvmunmap(myproc()->pagetable, addr + i, 1, 1);
  }

  // Update the VMA interval.
  if(addr == v->addr){
    v->addr += PGROUNDUP(len);
    v->offset += PGROUNDUP(len);
    v->len = v->len > PGROUNDUP(len) ? v->len - PGROUNDUP(len) : 0;
  } else if(addr+len == v->addr+v->len){
    v->len -= len;
  } else
    panic("vmamunmap(): punch a hole in the middle of the region");

  // Check if the VMA is exhausted. If so, free it.
  if(!v->len)
    vmafree(v);
}

// Handle page fault in the VMA. The function is called from vmfault() if `va'
// belongs to a VMA. So there is no need to check this again.
//
// Return 0 on failure, physical addr otherwise.
uint64
vmafault(pagetable_t pagetable, uint64 va, int write)
{
  // YOUR CODE HERE.

  return 0;
}
