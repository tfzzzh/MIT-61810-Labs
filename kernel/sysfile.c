//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}


/*
void *mmap(void *addr, size_t len, int prot, int flags,
           int fd, off_t offset)
*/
uint64 sys_mmap(void) {
  // get input and check if input is valid
  uint64 len;
  int prot, flags, fd;
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  struct proc* self = myproc();

  if (len <= 0 || len % PGSIZE != 0) {
    printf("sys_mmap: len = %ld is not a valid number\n", len);
    return -1;
  }

  if (fd < 0 || fd >= NOFILE) {
    printf("sys_mmap: fd = %d is not a valid number\n", fd);
    return -1;
  }

  struct file * fptr = self->ofile[fd];
  if (fptr == NULL) {
    printf("sys_mmap: fd = %d is bind to null\n", fd);
    return -1;
  }

  // check file type being f->type == FD_INODE
  // check file's offset is 0
  if (fptr->type != FD_INODE) {
    printf("sys_mmap: fd = %d is not bind to a file\n", fd);
    return -1;
  }

  // check RD compitible
  if ((prot & PROT_WRITE) && (flags & MAP_SHARED) && (fptr->writable == 0)) {
    printf("sys_mmap: fd = %d is not writable\n", fd);
    return -1;
  }


  // find a unused va
  int slot = 0;
  while (slot < NVMA && self->vm_areas[slot].length != 0) {
    slot += 1;
  }

  if (slot >= NVMA) {
    printf("sys_mmap: vm_areas are full\n");
    return -1;
  }
  struct vm_area *area = self->vm_areas + slot;

  // add reference to the fd, update file reference, one shall check if fd is valid
  // when fail one must call close
  filedup(fptr);
  area->fptr = fptr; // release when fail

  // get current proc, and use proc->size to find a unused region
  // we shall make start pasize alignment
  if (self->next_start == 0) {
    self->next_start = PGROUNDUP(self->sz);
  }
  if (self->next_start < self->sz) panic("sys_mmap: memory crash");
  uint64 start = self->next_start;
  uint64 end = start + len;
  if (end <= start) {
    printf("sys_mmap: address overflow\n");
    area->fptr = NULL;
    fileclose(fptr);
    return -1;
  }
  self->next_start = end;

  // add a VMA to the process's table of mapped regions
  area->start_addr = start;
  area->length = len;
  area->flags = flags;
  area->prot = prot;
  area->valid_start = start;
  area->valid_end = start + len;

  printf("sys_mmap size of proc: %lx, start_addr: %lx, end_addr: %lx\n", self->sz, start, start + len);
  // fill other part of the vma
  return start;
}


// int munmap(void *addr, size_t len);
void write_back(pagetable_t pagetable, struct file * fptr, uint64 addr, uint64 len, uint64 offset);
void put_back(pagetable_t pagetable, uint64 addr, uint64 len);
uint64 sys_munmap(void) {
  // parse parameters
  uint64 addr, len;
  argaddr(0, &addr);
  argaddr(1, &len);

  // addr and len must aligned with page size
  if (addr % PGSIZE != 0 || len % PGSIZE != 0) {
    printf("sys_munmap: addr or len not page aligned \n");
    return -1;
  }
  
  // get the memory range to release
  int slot = 0;
  struct proc * proc = myproc();
  struct vm_area * area = proc->vm_areas;
  while (slot < NVMA) {
    if (
      area->length != 0 &&
      area->valid_start <= addr &&
      addr + len <= area->valid_end
    ) {
      break;
    }
    slot += 1;
    area += 1;
  }

  // check if vma found
  if (slot >= NVMA) {
    printf("sys_munmap: range not in vm area\n");
    return -1;
  }


  // change valid_start or valid_end
  // [addr, addr+len] removed
  if (area->valid_start == addr) {
    area->valid_start = addr + len;
  }
  // [valid_end - len, valid_end] removed
  else if (addr + len == area->valid_end) {
    area->valid_end -= len;
  }
  else {
    printf("sys_munmap: cannot make a whole in vm area\n");
    return -1;
  }

  // write the page back to memory 
  if (area->flags & MAP_SHARED) {
    uint64 offset = addr - area->start_addr;
    // printf("clear page range [%lx, %lx) in [%lx, %lx]\n", addr, addr+len, area->start_addr, area->start_addr + area->length);
    write_back(proc->pagetable, area->fptr, addr, len, offset);
  }
  else {
    // unintall pages
    put_back(proc->pagetable, addr, len);
  }

  // when all memory release one should release vm area
  if (area->valid_start >= area->valid_end) {
    clear_vm_area(area, proc->pagetable);
  }

  return 0;
}


/*
// 1. if share bit set sync with disk
// 2. decrement the reference count of the corresponding struct file
// 3. clear the whole slot
*/
void clear_vm_area(struct vm_area * area, pagetable_t pagetable) {
  struct file * fptr = area->fptr;
  // printf("clear page range [%lx, %lx) in [%lx, %lx]\n", area->valid_start, area->valid_end, area->start_addr, area->start_addr + area->length);
  // sync with disk
  if (area->valid_start < area->valid_end) {
    uint64 addr = area->valid_start;
    uint64 len = area->valid_end - area->valid_start;
    if (area->flags & MAP_SHARED) {
      uint64 offset = addr - area->start_addr;
      write_back(pagetable, area->fptr, addr, len, offset);
    }
    else {
      put_back(pagetable, addr, len);
    }
  }

  fileclose(fptr);

  // clear the slot
  memset(area, 0, sizeof(struct vm_area));
}


// assumption: ip is not locked
// handle share case
void write_back(pagetable_t pagetable, struct file * fptr, uint64 addr, uint64 len, uint64 offset) {
  // sync
  struct inode * ip = fptr->ip;
  begin_op();
  ilock(ip);

  /// assumption offset is page aligned
  if (offset % PGSIZE != 0) panic("write_back: pagsize not aligned");
  // while (ip->size < offset) {
  //   if (writei(ip, 0, (uint64) zpg, ip->size, PGSIZE) != PGSIZE) {
  //     panic("write_back: writei zero page fail");
  //   }
  // }

  // what if two process share a file? may need lock
  for (uint64 pgaddr = addr; pgaddr < addr+len; pgaddr += PGSIZE) {
    // check if pgaddr loaded into the table, if yes write it back to disk (check dirty)
    if (walkaddr(pagetable, pgaddr) != 0) {
      pte_t* pte = walk(pagetable, pgaddr, 0);
      if (
        (PTE_FLAGS(*pte) & PTE_D) &&
        offset < ip->size
      )
      {
        uint nbyte = ip->size - offset;
        // printf("try to write back for %d bytes\n", nbyte);
        if (writei(ip, 0, PTE2PA(*pte), offset, nbyte) != nbyte) {
          panic("write_back: writei fail");
        }
      }

      // this page can remove
      uvmunmap(pagetable, pgaddr, 1, 1);

      offset += PGSIZE;
    }
  }
  iunlock(ip);
  end_op();
}

// handle private case
void put_back(pagetable_t pagetable, uint64 addr, uint64 len) {
  for (uint64 pgaddr = addr; pgaddr < addr+len; pgaddr += PGSIZE) {
    // check if pgaddr loaded into the table, if yes write it back to disk (check dirty)
    if (walkaddr(pagetable, pgaddr) != 0) {
      // this page can remove
      uvmunmap(pagetable, pgaddr, 1, 1);
    }
  }
}

int mmap_load_instr() {
  if (r_scause() != 0xd && r_scause() != 0xf) return -1; // failed
  uint64 va = r_stval();

  // get current proc and its pagetable
  struct proc * proc = myproc();
  pagetable_t pagetable = proc->pagetable;

  // get the page addr for va
  uint64 pgaddr = PGROUNDDOWN(va);

  // find the vm_area contains pgaddr
  int slot = 0;
  struct vm_area * area = proc->vm_areas;
  while (slot < NVMA) {
    if (
      area->length != 0 &&
      area->start_addr <= pgaddr &&
      pgaddr < area->start_addr + area->length
    ) {
      break;
    }
    slot += 1;
    area += 1;
  }

  if (slot >= NVMA) {
    printf("mmap_load_instr: va %lx not in any vm area\n", va);
    return -1;
  }

  // va not in valid range ? panic
  if (va < area->valid_start || va >= area->valid_end) {
    printf("mmap_load_instr: visit invalid memory\n");
    exit(-1);
  }

  // write an only readable page
  if ((area->prot & PROT_WRITE) == 0 && r_scause() == 0xf) {
    printf("try to write a readable page\n");
    exit(-1);
  }

  // read priority from vma
  int perm = PTE_U;
  if ((area->prot & PROT_READ)) {
    perm = perm | PTE_R;
  }
  if ((area->prot & PROT_WRITE)) {
    perm = perm | PTE_W;
  }
  // X and None shall not supprted

  // allocate a page for the pgaddr and map it to user pagetable
  void * newpage = kalloc();
  if (newpage == NULL) {
    printf("mmap_load_instr: memory full\n");
    return -1;
  }

  memset(newpage, 0, PGSIZE);

  printf("map page addr: %lx\n", pgaddr);
  if (mappages(pagetable, pgaddr, PGSIZE, (uint64) newpage, perm) == -1) {
    // resources: newpage
    kfree(newpage);
    printf("mmap_load_instr: map page not success\n");
    return -1;
  }

  printf("mappages done\n");

  uint64 offset = pgaddr - area->start_addr; // not shrink start_add in unmmap
  int read_bytes = 0;
  struct inode * ip = area->fptr->ip;
  ilock(ip);
  // case 1: offset >= filesize, noting need to read
  if (offset < ip->size) {
    // case 2: offset < filesize, but offset + pagesize > filesize
    // case 3: offset < filesize && offset + pagesize <= filesize
    // load data from the file

    if ((read_bytes = readi(area->fptr->ip, 0, (uint64) newpage, offset, PGSIZE)) == 0) {
      // failed release resources
      iunlock(area->fptr->ip);
      printf("mmap_load_instr: read fail from the inode");
      uvmunmap(pagetable, pgaddr, 1, 1);
      return -1;
    }
  }

  iunlock(area->fptr->ip);
  return 0;
}