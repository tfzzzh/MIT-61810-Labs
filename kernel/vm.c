#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
#ifdef LAB_PGTBL
      if(PTE_LEAF(*pte)) {
        return pte;
      }
#endif
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// walk and allocate for superpage, when failed null is returned
pte_t *
walk_alloc_supage(pagetable_t pagetable, uint64 va)
{
  // printf("va = %p\n", (void*) va);
  if(va >= MAXVA)
    panic("walk_alloc_supage");

  // level 2
  pte_t entry = pagetable[PX(2, va)];
  if (!(entry & PTE_V)) {
    // alloc a level 1 pagetable
    void * mem = kalloc();
    if (mem == 0) return 0;
    memset(mem, 0, PGSIZE);
    printf("mem = %p\n", pagetable);

    // pack entry with format pa | flag
    entry = SPPA2PTE(((pte_t) mem));
    entry = entry | PTE_V;

    // set value
    pagetable[PX(2, va)] = entry;
  }

  // level 1 table
  pagetable = (pagetable_t) PTE2PA(entry);
  // printf("level1 page table = %p\n", pagetable);

  // pte for va shall stored in the level 1 pagetable
  return &(pagetable[PX(1, va)]);
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}


// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  short require_simple_page = size > PGSIZE;
  if (require_simple_page && pa >= SUPER_PAGE_LIST_START)
    panic("mappages: require simple page");

  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0) // now page [va + i*size] mapped to [pa + i*size]
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// map an address in superpage, when failed return -1
int
mapsuperpage(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  // va must algined with superpage
  if (va % SUPERPGSIZE != 0)
    panic("mapsuperpage: va not aligned");

  // pa mus algined with superpage
  if (pa % SUPERPGSIZE != 0)
    panic("mapsuperpage: pa not aligned");
  
  // find pte to store the virtual address
  pte_t *pte = walk_alloc_supage(pagetable, va);
  if (pte == 0) return -1;

  if (*pte & PTE_V)
    panic("mapsuperpage: remap");

  *pte = SPPA2PTE(pa) | perm | PTE_V;

  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  int sz;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  short require_short_page = npages > 1;

  for(a = va; a < va + npages*PGSIZE; a += sz){
    sz = PGSIZE;
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0) {
      printf("va=%ld pte=%ld\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if (require_short_page) {
      uint64 pa = PTE2PA(*pte);
      if (pa >= SUPER_PAGE_LIST_START) panic("uvmunmap: require simple page");
    }
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// unmap one superpage from the table
void
uvmunmap_super(pagetable_t pagetable, uint64 va, int do_free)
{
  // check whether va is superpage aligned
  if((va % SUPERPGSIZE) != 0)
    panic("uvmunmap_super: not aligned");

  // find pte for va
  pte_t * pte = pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmunmap_super: walk");

  // check if the entry is valid
  // check if the page is leaf
  if((*pte & PTE_V) == 0) {
    printf("va=%ld pte=%ld\n", va, *pte);
    panic("uvmunmap_super: not mapped");
  }
  if(PTE_FLAGS(*pte) == PTE_V)
    panic("uvmunmap_super: not a leaf");
  
  // free the page
  if (do_free)
  {
    uint64 pa = SPPTE2PA((*pte));
    // check if pa in the valid range
    if (!(pa >= SUPER_PAGE_LIST_START && pa < SUPER_PAGE_LIST_END))
      panic("uvmunmap_super: out of range");
    // collect pa
    superfree((void *) pa);
  }

  // invalidate the entry
  *pte = 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error. (backup)
// uint64
// uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
// {
//   char *mem;
//   uint64 a;
//   int sz;

//   if(newsz < oldsz)
//     return oldsz;

//   oldsz = PGROUNDUP(oldsz);
//   for(a = oldsz; a < newsz; a += sz){
//     sz = PGSIZE;
//     mem = kalloc();
//     if(mem == 0){
//       uvmdealloc(pagetable, a, oldsz);
//       return 0;
//     }
// #ifndef LAB_SYSCALL
//     memset(mem, 0, sz);
// #endif
//     if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
//       kfree(mem);
//       uvmdealloc(pagetable, a, oldsz);
//       return 0;
//     }
//   }
//   return newsz;
// }

// strategy: when allocate superpage
short can_use_superpage(uint64 start, uint64 end) {
  return (start % SUPERPGSIZE == 0) && (start + SUPERPGSIZE <= end);
}


uint64
uvmalloc_one_page(pagetable_t pagetable, uint64 oldsz, uint64 sz, int xperm)
{
  // uint64 sz = PGSIZE;
  void * mem = sz == PGSIZE ? kalloc() : superalloc();
  if(mem == 0){
    return 0;
  }
  memset(mem, 0, sz);

  // try to bind page to pagetable
  int mp_status = 
  sz == PGSIZE?
    mappages(pagetable, oldsz, sz, (uint64)mem, PTE_R|PTE_U|xperm):
    mapsuperpage(pagetable, oldsz, (uint64) mem, PTE_R|PTE_U|xperm);

  // failed
  if(mp_status != 0){
    sz == PGSIZE ? kfree(mem) : superfree(mem);
    return 0;
  }

  return (uint64) mem;
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// I add logic when current range can be filled with superpage then a superpage
// is used
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += sz){
    if (can_use_superpage(a, newsz)) {
      sz = SUPERPGSIZE;
      if (uvmalloc_one_page(pagetable, a, sz, xperm) == 0) {
        sz = PGSIZE;
        if (uvmalloc_one_page(pagetable, a, sz, xperm) == 0) {
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
      }
    }
    else {
      sz = PGSIZE;
      if (uvmalloc_one_page(pagetable, a, sz, xperm) == 0) {
        uvmdealloc(pagetable, a, oldsz);
        return 0;
      }
    }
  }
  return newsz;
}

// (backup)
// // Deallocate user pages to bring the process size from oldsz to
// // newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// // need to be less than oldsz.  oldsz can be larger than the actual
// // process size.  Returns the new process size.
// uint64
// uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
// {
//   if(newsz >= oldsz)
//     return oldsz;

//   if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
//     int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
//     uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
//   }

//   return newsz;
// }

// query physical page, the va shall exist
uint64 query_physical_page_addr(pagetable_t pagetable, uint64 va)
{
  pte_t * pte = walk(pagetable, va, 0);
  if (pte == 0) {
    panic("query_physical_page_addr: va not exist");
  }

  return PTE2PA((*pte));
}


short is_super_page(uint64 pa)
{
  return pa >= SUPER_PAGE_LIST_START && pa < SUPER_PAGE_LIST_END; 
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;
  
  // newsz < oldsz && newsz point to a valid region
  uint64 va = newsz;
  while (va < oldsz)
  {
    // get the physical page contains va
    uint64 pa = query_physical_page_addr(pagetable, va);

    // this page can be removed if va is aligned
    // update va by va += pagesize or align it
    short is_super = is_super_page(pa);
    int size = is_super ? SUPERPGSIZE : PGSIZE;
    
    // aligned
    if (va % size == 0) {
      // remove the page pointed by va from the table
      if (is_super)
        uvmunmap_super(pagetable, va, 1);
      else 
        uvmunmap(pagetable, va, 1, 1);
      va += size;
    }
    // not aligned
    else {
      va = is_super ? (SUPERPGROUNDUP(va)) : (PGROUNDUP(va));
    }
  }

  // if va > oldsz -> the page contains oldsz is considered
  // if va == oldsz -> oldsz must not point to a valid page

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    // uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
    uvmdealloc(pagetable, sz, 0);
  freewalk(pagetable);
}

// // Given a parent process's page table, copy
// // its memory into a child's page table.
// // Copies both the page table and the
// // physical memory.
// // returns 0 on success, -1 on failure.
// // frees any allocated pages on failure.
// int
// uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
// {
//   pte_t *pte;
//   uint64 pa, i;
//   uint flags;
//   char *mem;
//   int szinc;

//   for(i = 0; i < sz; i += szinc){
//     szinc = PGSIZE;
//     szinc = PGSIZE;
//     if((pte = walk(old, i, 0)) == 0)
//       panic("uvmcopy: pte should exist");
//     if((*pte & PTE_V) == 0)
//       panic("uvmcopy: page not present");
//     pa = PTE2PA(*pte);
//     flags = PTE_FLAGS(*pte);
//     if((mem = kalloc()) == 0)
//       goto err;
//     memmove(mem, (char*)pa, PGSIZE);
//     if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
//       kfree(mem);
//       goto err;
//     }
//   }
//   return 0;

//  err:
//   uvmunmap(new, 0, i / PGSIZE, 1);
//   return -1;
// }


// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  int szinc;

  for(i = 0; i < sz; i += szinc){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    short is_super = is_super_page(pa);
    if (!is_super) {
      szinc = PGSIZE;
      if((mem = kalloc()) == 0)
        goto err;
      memmove(mem, (char*)pa, PGSIZE);
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto err;
      }
    }
    else {
      szinc = SUPERPGSIZE;
      if ((mem = superalloc()) == 0)
        goto err;
      memmove(mem, (char*)pa, SUPERPGSIZE);
      if (mapsuperpage(new, i, (uint64)mem, flags) == -1) {
        superfree(mem);
        goto err;
      }
    }
  }
  return 0;

 err:
  // uvmunmap(new, 0, i / PGSIZE, 1); remove all allocated pages
  uvmdealloc(new, i, 0);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;
    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist 0x%x %d\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}


#ifdef LAB_PGTBL
void _vmprint(uint64 pageaddr, uint64 virtualaddr, int level)
{
  pagetable_t pagetable = (pagetable_t) pageaddr;
  for (int i=0; i < 512; ++i) {
    pte_t pte = pagetable[i];
    if (pte & PTE_V) {
      uint64 ua = virtualaddr + 
        ((uint64) i) * (1L << (PGSHIFT + 9 * (3-level)));
      
      uint64 subpage = PTE2PA(pte);

      for(int l=0; l < level; ++l) printf(" ..");
      printf("%p: pte %p pa %p\n", (void *)ua, (void*) pte, (void*) subpage);

      if (!PTE_LEAF(pte)) {
        _vmprint(subpage, ua, level+1);
      }
    }
  }
}

void
vmprint(pagetable_t pagetable) {
  // your code here
  printf("page table %p\n", pagetable);

  // visit root
  for (int i=0; i < 512; ++i) {
    pte_t pte = pagetable[i];
    // check if pte valid
    if (pte & PTE_V) {
      // start virtual address of this slot
      uint64 va = ((uint64) i) * (1L << (PGSHIFT + 9 * 2));

      // get page address of next level
      uint64 subpage = PTE2PA(pte);

      // print level1, pte, pa
      printf(" ..%p: pte %p pa %p\n", (void *)va, (void*) pte, (void*) subpage);

      // recursively print at the address
      _vmprint(subpage, va, 2);
    }
  }
}
#endif



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif
