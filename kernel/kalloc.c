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
void superfreerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem, kmem_super;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  // freerange(end, (void*)PHYSTOP);
  freerange(end, (void*)ORD_PAGE_LIST_END);

  // init super page list
  initlock(&kmem_super.lock, "kmem_super");
  // assumption the start and end shall be superpage aligned
  if (
    SUPER_PAGE_LIST_START % SUPERPGSIZE != 0 ||
    SUPER_PAGE_LIST_END % SUPERPGSIZE != 0
  )
    panic("kinit: super page list start and end not 2MB aligned\n");
  superfreerange((void *) SUPER_PAGE_LIST_START, (void *) SUPER_PAGE_LIST_END);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// free range of super page
void
superfreerange(void *pa_start, void *pa_end)
{
  char *p = (char*)SUPERPGROUNDUP((uint64)pa_start);
  for(; p + SUPERPGSIZE <= (char*)pa_end; p += SUPERPGSIZE)
    superfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= ORD_PAGE_LIST_END)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// free a super page
void
superfree(void *pa)
{
  if(
    ((uint64)pa % SUPERPGSIZE) != 0 || 
    (uint64) pa < SUPER_PAGE_LIST_START || 
    (uint64) pa >= SUPER_PAGE_LIST_END)
    panic("superfree: invalid free range");

  memset(pa, 1, SUPERPGSIZE);

  struct run * r = (struct run*)pa;

  acquire(&kmem_super.lock);
  r->next = kmem_super.freelist;
  kmem_super.freelist = r;
  release(&kmem_super.lock);
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
  return (void*)r;
}


// allocate a supper page
void *
superalloc(void)
{
  acquire(&kmem_super.lock);
  struct run * r = kmem_super.freelist;
  if(r)
    kmem_super.freelist = r->next;
  release(&kmem_super.lock);

  if(r)
    memset((char*)r, 5, SUPERPGSIZE); // fill with junk
  return (void*)r;
}