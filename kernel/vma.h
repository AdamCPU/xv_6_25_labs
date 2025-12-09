#ifndef __VMA__
#define __VMA__

struct file;

struct vma {
  // VMA can be either in system VMAs freelist or in a process VMAs list.
  struct vma *next;

  // Set by vmaalloc().
  uint64 addr;
  uint64 len;
  struct file *fd;

  // Set by user.
  int prot;
  int flags;
  uint64 offset;
};

#endif
