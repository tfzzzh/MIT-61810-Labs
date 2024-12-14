// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void giveback(void *pa);
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

/*
 * reference count array
 */
#define MAX_NUM_PAGE 32768
char reference_counts[MAX_NUM_PAGE];
struct spinlock rc_lock;

// initalize reference counts for pages
void init_pg_rc() {
  memset(reference_counts, 0, sizeof(reference_counts));
  initlock(&rc_lock, "rc_lock");
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  init_pg_rc();
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  // int num_pages = ((PGROUNDUP((uint64)pa_end)) - PGROUNDUP((uint64)pa_start)) / PGSIZE;
  // printf("total free pages = %d\n", num_pages);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    giveback(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // reduce reference count
  int rc;
  acquire(&rc_lock);
  if (reference_counts[PGADDR2IDX(pa)] <= 0) panic("kfree: double free");
  reference_counts[PGADDR2IDX(pa)] -= 1;
  rc = reference_counts[PGADDR2IDX(pa)];
  release(&rc_lock);

  if (rc == 0) {
    // Fill with junk to catch dangling refs.
    giveback(pa);
  }
}

// give back memory to the pool (real free)
void
giveback(void *pa)
{
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  struct run * r = (struct run*)pa;

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
  // printf("kalloc start\n");
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  // special case: not enough memory
  if (r == 0) return (void *) r;

  // change reference count to 1
  acquire(&rc_lock);
  // printf("here1 r = %ld\n", (uint64) r);
  if (reference_counts[PGADDR2IDX(r)] != 0) {
    panic("kalloc: reference count not 0");
  }
  reference_counts[PGADDR2IDX(r)] = 1;
  release(&rc_lock);

  memset((char*)r, 5, PGSIZE); // fill with junk
  // printf("kalloc end\n");
  return (void*)r;
}

// alloc without set rc and fill
void * kalloc_no_rc(void) {
  struct run *r = kmem.freelist;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  return r;
}

pte_t
shallow_copy(pte_t pte)
{
  pte_t pte_new = pte;
  pte_new = TRANS_SET_S(pte_new);
  
  // if a page is writable, set PTE_SW and unset PTE_W
  if (
    (pte & PTE_W) || (pte & PTE_SW)
  )
  {
    pte_new = TRANS_SET_SW(pte_new);
    pte_new = TRANS_CLR_W(pte_new);
  }

  // add refcount (add lock)
  uint64 pa = PTE2PA(pte);
  acquire(&rc_lock);
  reference_counts[PGADDR2IDX(pa)] += 1;
  release(&rc_lock);

  return pte_new;
}

// when failed return -1
int handle_cow_trap(pte_t * pte_ref) {
  if (pte_ref == NULL) {
    exit(-1);
    panic("handle_cow_trap: cannot handle page error, since va not exist");
  }

  // check if the pte is valid and writable
  if (!ISPAGE_VALID(*pte_ref)) {
    exit(-1);
    panic("handle_cow_trap: cannot handle page error, since va point to invalid page");
  }
  if (!ISPAGE_SHARED(*pte_ref)) {
    exit(-1);
    panic("handle_cow_trap: cannot handle page error, since page not shared");
  }
  if (!ISPAGE_WRITABLE(*pte_ref)) {
    exit(-1);
    panic("handle_cow_trap: cannot handle page error, page not writtable");
  }

  uint64 pa = PTE2PA(*pte_ref);
  uint64 pte_new = *pte_ref;

  // change this page to exclusive
  // set share bit to 0
  // set share write to 0
  // set the page to writable
  pte_new = TRANS_CLR_S(pte_new);
  pte_new = TRANS_CLR_SW(pte_new);
  pte_new = TRANS_SET_W(pte_new);

  acquire(&rc_lock);
  // acquire(&kmem.lock);

  if (reference_counts[PGADDR2IDX(pa)] > 1) {
    // reduce ref count
    reference_counts[PGADDR2IDX(pa)] -= 1;

    // allocate a new memory (may fail)
    void * mem = kalloc_no_rc(); // here we need to obtain freelist lock
    if (mem == NULL) {
      release(&rc_lock);
      return -1;
    }
    // fill the memory
    memmove(mem, (void*) pa, PGSIZE);
    // change pte_new's address content
    pte_new = PA2PTE((uint64) mem) | (PTE_FLAGS(pte_new));

    reference_counts[PGADDR2IDX(mem)] = 1;
  }
  // release(&kmem.lock);
  release(&rc_lock);

  // when not exclusive perform memcpy
  // can this page changed? during copy (y)
  // set *pte
  *pte_ref = pte_new;
  return 0;
}