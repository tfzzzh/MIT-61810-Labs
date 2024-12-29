# MIT 6.1810: Operating System Engineering - Lab Solutions
This repository contains all solutions to the labs from MIT course: [6.1810 Operating
System Engineering](https://pdos.csail.mit.edu/6.828/2024/schedule.html).
## My Acknowledgments
I would like to thank **Professor Robert Morris**, **Professor Adam Belay** and the entire teaching staff for sharing this incredible course to the public. This course significantly enhances my understand of both the principles and the practical implementation of the Unix kernel. 
## Environment Setup
The labs requires develop tools including **QEMU, GDB, GCC**. You can
check how to install them on the course's [website](https://pdos.csail.mit.edu/6.1810/2024/tools.html).

For my setup, all the labs are completed on **Ubuntu 24.04**.
## Reproduce Solutions
To reproduce the solutions for each lab, follow the steps below for each respective lab:
### Lab: Xv6 and Unix utilities
```shell
git fetch
git checkout util
make clean
make grade
```
### Lab: system calls
```shell
git fetch
git checkout syscall
make clean
make grade
```
### Lab: page tables
```shell
git fetch
git checkout pgtbl
make clean
make grade
```
### Lab: traps
```shell
git fetch
git checkout traps
make clean
make grade
```
### Lab: Copy-on-Write Fork for xv6
```shell
git fetch
git checkout cow
make clean
make grade
```
### Lab: networking
```shell
git fetch
git checkout net
make clean
make grade
```
### Lab: locks
```shell
git fetch
git checkout lock
make clean
make grade
```
### Lab: file system
```shell
git fetch
git checkout fs
make clean
make grade
```
### Lab: mmap
```shell
git fetch
git checkout mmap
make clean
make grade
```

## README For Xv6 Project


xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  xv6 loosely follows the structure and style of v6,
but is implemented for a modern RISC-V multiprocessor using ANSI C.

ACKNOWLEDGMENTS

xv6 is inspired by John Lions's Commentary on UNIX 6th Edition (Peer
to Peer Communications; ISBN: 1-57398-013-7; 1st edition (June 14,
2000)).  See also https://pdos.csail.mit.edu/6.1810/, which provides
pointers to on-line resources for v6.

The following people have made contributions: Russ Cox (context switching,
locking), Cliff Frey (MP), Xiao Yu (MP), Nickolai Zeldovich, and Austin
Clements.

We are also grateful for the bug reports and patches contributed by
Takahiro Aoyagi, Marcelo Arroyo, Silas Boyd-Wickizer, Anton Burtsev,
carlclone, Ian Chen, Dan Cross, Cody Cutler, Mike CAT, Tej Chajed,
Asami Doi,Wenyang Duan, eyalz800, Nelson Elhage, Saar Ettinger, Alice
Ferrazzi, Nathaniel Filardo, flespark, Peter Froehlich, Yakir Goaron,
Shivam Handa, Matt Harvey, Bryan Henry, jaichenhengjie, Jim Huang,
Matúš Jókay, John Jolly, Alexander Kapshuk, Anders Kaseorg, kehao95,
Wolfgang Keller, Jungwoo Kim, Jonathan Kimmitt, Eddie Kohler, Vadim
Kolontsov, Austin Liew, l0stman, Pavan Maddamsetti, Imbar Marinescu,
Yandong Mao, Matan Shabtay, Hitoshi Mitake, Carmi Merimovich, Mark
Morrissey, mtasm, Joel Nider, Hayato Ohhashi, OptimisticSide,
phosphagos, Harry Porter, Greg Price, RayAndrew, Jude Rich, segfault,
Ayan Shafqat, Eldar Sehayek, Yongming Shen, Fumiya Shigemitsu, snoire,
Taojie, Cam Tenny, tyfkda, Warren Toomey, Stephen Tu, Alissa Tung,
Rafael Ubal, Amane Uehara, Pablo Ventura, Xi Wang, WaheedHafez,
Keiichi Watanabe, Lucas Wolf, Nicolas Wolovick, wxdao, Grant Wu, x653,
Jindong Zhang, Icenowy Zheng, ZhUyU1997, and Zou Chang Wei.

ERROR REPORTS

Please send errors and suggestions to Frans Kaashoek and Robert Morris
(kaashoek,rtm@mit.edu).  The main purpose of xv6 is as a teaching
operating system for MIT's 6.1810, so we are more interested in
simplifications and clarifications than new features.

BUILDING AND RUNNING XV6

You will need a RISC-V "newlib" tool chain from
https://github.com/riscv/riscv-gnu-toolchain, and qemu compiled for
riscv64-softmmu.  Once they are installed, and in your shell
search path, you can run "make qemu".