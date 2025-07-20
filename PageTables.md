# Lab：Page Tables

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

在`make qemu`的时候，需要修改`makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

和前两个LAB不同，观察实现代码可知，本实验由于涉及到较多的内核代码，因此很多代码使用宏来开启，因此本实验不需要修改`user.h`和`usys.pl`的内容。因此本实验通过`make qemu DLAB_PGTBL`来编译运行

## Speed up system calls

简要介绍：在这个部分中，普通的系统调用是通过在发起系统调用的时候，将上下文保存在页`TRAPFRAME`中，然后调用函数从这个页中读取所需要的参数。这样效率不如直接给定一个映射好的共享内存页面，因此本部分主要想法就是用共享内存的方式来提高getpid的效率。

首先，在PCB中要添加一个指向这个页面的指针`struct usyscall *usyscall;`，这个结构体的定义在`kernel/riscv.h`中

```c

// Per-process state
struct proc {
  struct spinlock lock;
  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID
  struct usyscall *usyscall;   
  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process
  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};
```

 然后为这个页面初始化，在`kernel/proc.c`中

首先是在页面分配的函数`static struct proc* allocproc(void)`中，添加对usyscall页面的初始化

```c
found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  if((p->usyscall = (struct usyscall *)kalloc()) == 0){//这里使用了kalloc来分配一个页面，同样的也要做好错误处理
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  // memset(p->usyscall, 0, PGSIZE);
  p->usyscall->pid = p->pid;//保存pid

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
```

然后修改`pagetable_t proc_pagetable(struct proc *p)`来将分配的页面映射到用户空间

```c
  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }
  if(mappages(pagetable, USYSCALL, PGSIZE, (uint64)p->usyscall, PTE_R | PTE_U)<0){//将分配的物理页面映射到当前进程的用户页表
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }
```

注意到内存的申请就一定伴随着释放，在上面映射那么在释放的时候接触物理页到用户空间的映射。

```c
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);//执行用户/内核切换的ecall汇编代码映射
  uvmunmap(pagetable, TRAPFRAME, 1, 0);//切换内核态时上下文保存的页面
  uvmunmap(pagetable, USYSCALL, 1, 0);
  uvmfree(pagetable, sz);
}
```

同时在`static void freeproc(struct proc *p)`添加物理页的释放

```c
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;

  if(p->usyscall)//和trapframe的释放方式是一致的
    kfree((void*)p->usyscall);
  p->usyscall = 0;
  
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}
```

## Print a page table

查阅资料可知，XV6采用的是**RISC-V Sv39** 页表格式，它是一种三级页表的设计格式。虚拟地址的结构如下：前27位是用来三级页表索引，后12位则是偏移量

| 9 bits | 9 bits | 9 bits | 12 bits     |
| ------ | ------ | ------ | ----------- |
| VPN[2] | VPN[1] | VPN[0] | page offset |
| L2     | L1     | L0     | Offset      |

这里打印页表，首先在`kernel/riscv.h`中添加宏，来表示一个页中有多少个页表项

```c
#define PTENTRIES (1 << 9)  // 2^9 = 512 PTEs per page table
```

然后在`kernel/vm.c`中添加一下内容

```c
void vmprint(pagetable_t pagetable){
  printf("page table %p\n", pagetable);
  vmprint_helper(pagetable, 0);
}

void vmprint_helper(pagetable_t pagetable, int level){
  for(int i = 0; i < PTENTRIES; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){

      if(level == 0)printf("..");
      else if(level == 1)printf(".. ..");
      else printf(".. .. ..");

      printf("%d: ", i);
      uint64 children = PTE2PA(pte);
      printf("pte %p pa %p\n", pte, children);

      if(level < 2){
        vmprint_helper((pagetable_t)children, level + 1);
      }
    }
  }
}
```

这里采用了递归的思想，来打印所有合法的页表项与之对应的物理地址。

## Detecting which pages have been accessed

简要介绍：这个部分主要添加一个系统调用，起作用是检测进程所使用的页表中，哪些是被使用过的。

首先在xv6中，在虚拟地址的offset的12位部分，添加宏`PTE_A`,`PTE_D`来表明页是否可访问以及是否被使用过。

在`kernel/riscv.h`中添加

```c
#define PTE_A (1L << 6) // Accessed
#define PTE_D (1L << 7) // Dirty
```

阅读`kernel/syscall.c`和`kernel/syscall.h`中的代码可知，以及添加了`sys_pgaccess`的系统调用号和对应的函数指针映射，因此只要去`kernel/sysproc.c`中添加对`sys_pgaccess`的实现即可

```c
#ifdef LAB_PGTBL
int sys_pgaccess(void){
  uint64 addr;
  if(argaddr(0, &addr) < 0)
    return -1;
  int n;
  if(argint(1, &n) < 0)
    return -1;
  uint64 buf;
  if(argaddr(2, &buf) < 0)
    return -1;
  return pgaccess((void *)addr, n, (void *)buf);
}
#endif
```

可以看到，`sys_pgaccess`在读取了页`TRAPFRAME`中的参数后，调用了`pgaccess`。为什么要使用`argaddr`接受参数呢？

可以看到在`user/user.h`中
```c
#ifdef LAB_PGTBL
int pgaccess(void *base, int len, void *mask);
// usyscall region
int ugetpid(void);
#endif
```

已经定义了传递参数的类型，这里处于一致性，后面也进行了类型的转化。（注意这里的`pgaccess`和`sys_pgaccess`中后面调用的`pgaccess`没有任何关系）

接下来在`kernel/proc.c`中实现系统调用实现中最后调用的`pgaccess`

```c
#ifdef LAB_PGTBL
int pgaccess(void *addr, int n, void *buf){
  pagetable_t pagetable = myproc()->pagetable;
  uint64 bitmask;
  for(int i =0 ; i < n; ++i){
    pte_t* pte = walk(pagetable, (uint64)addr + i * PGSIZE, 0);
    if(*pte && (*pte & PTE_A)){
      bitmask |= (1<<i);
      *pte ^= PTE_A;
    }
  }
  if(copyout(pagetable, (uint64)buf, (char *)&bitmask, sizeof(uint64)) < 0){
    return -1;
  }
  return 0;
}
#endif
```

## 总结

xv6-2021fall 的页表实验实现了基于 RISC-V Sv39 格式的三级页表结构，涉及虚拟地址到物理地址的多级转换机制。通过 `walk` 实现页表项查找与可选分配，`mappages` 用于建立虚实映射，`uvmalloc` 完成用户内存空间扩展，`copyout` 和 `copyin` 实现内核与用户态的数据传输，`vmprint` 可视化页表结构。通过实现该部分，可以更好的理解操作系统中页表是怎么实际管理的