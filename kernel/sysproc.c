#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;
  backtrace();
  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// alarm
uint64
sys_sigalarm(void)
{
  int tick;
  argint(0, &tick);

  uint64 func_ptr_val;
  argaddr(1, &func_ptr_val);

  // printf("accept tick=%d, func_ptr=%p", tick, (void*) func_ptr_val);
  struct proc * self = myproc();
  acquire(&self->lock);
  self->alarm_ticks = tick;
  self->alarm_handle = (void (*)()) func_ptr_val;
  self->alarm_elapsed = 0;
  release(&self->lock);
  return 0;
}

uint64
sys_sigreturn(void)
{
  // restore trapframe
  struct proc * self = myproc();
  if (self->trapframe != self->alarm_frame - 1)
    panic("sys_sigreturn");

  memmove(self->trapframe, self->alarm_frame, sizeof(struct trapframe));
  
  return 0;
}