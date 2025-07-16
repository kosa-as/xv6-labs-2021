# Lab：Sysinfo

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

在`make qemu`的时候，需要修改`makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

同时在`makefile`中添加用户的执行指令entry("trace");

entry("sysinfo");

```makefile
UPROGS=\
	$U/_trace\
	$U/_sysinfotest\
```

在本次的`lab`中，主要实现了两个系统调用分别是：`trace`和`sysinfo`。要通过编译，首先要在`user.h`中给出函数的申明

```c
struct sysinfo;
int trace(int);
int sysinfo(struct sysinfo*);
```

然后在`usys.pl`中添加（目的是在 xv6 中生成系统调用的汇编包装代码）

```perl
entry("trace");
entry("sysinfo");
```

只有在添加之后，才可以`make qemu`成功执行

## trace

简要介绍：trace的主要功能是跟踪检查系统调用，通过捕获用户传入的参数`mask`，来选择跟踪的发生系统调用的进程，并且打印进程的一些基本信息

首先要在PCB中添加mask字段，来标记发生的系统调用是什么类型的（==第15行==）。

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
  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process
  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  int mask;                    // Trace mask
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};
```

注意要在`kernel/syscall.h`中添加对应宏

```c
#define SYS_trace  22
```

同时，根据hint中：`Modify fork() (see kernel/proc.c) to copy the trace mask from the parent to the child process`，需要在`kernel/proc.c`中的`fork`函数添加对应字段的复制

```c
  //Modify fork() (see kernel/proc.c) to copy the trace mask from the parent to the child process. 
  np->mask = p->mask;
```

然后在`kernel/syscall.c`中添加，系统调用

```c
extern uint64 sys_trace(void);
```

同时在`(*syscalls[])(void)`函数指针数组中添加项目`[SYS_trace]   sys_trace,`

```c
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
...
[SYS_sysinfo] sys_trace,
};
```

由于要打印出所调用的系统调用的名称，因此添加了`*syscall_names`的声明。并且在`syscall`函数中修改了实现的部分，在每次成功执行`syscall`之后都会调用`trace`

```c
static char *syscall_names[] = {
  [SYS_fork]    "fork",
  [SYS_exit]    "exit",
  [SYS_wait]    "wait",
  [SYS_pipe]    "pipe",
  [SYS_read]    "read",
  [SYS_kill]    "kill",
  [SYS_exec]    "exec",
  [SYS_fstat]   "fstat",
  [SYS_chdir]   "chdir",
  [SYS_dup]     "dup",
  [SYS_getpid]  "getpid",
  [SYS_sbrk]    "sbrk",
  [SYS_sleep]   "sleep",
  [SYS_uptime]  "uptime",
  [SYS_open]    "open",
  [SYS_write]   "write",
  [SYS_mknod]   "mknod",
  [SYS_unlink]  "unlink",
  [SYS_link]    "link",
  [SYS_mkdir]   "mkdir",
  [SYS_close]   "close",
  [SYS_trace]   "trace",
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();
  
  num = p->trapframe->a7; // 获取系统调用号
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num](); // 执行对应的系统调用
    if(p->mask != 0 && p->mask & (1 << num)) {
      printf("%d: syscall %s -> %d\n", p->pid, syscall_names[num], p->trapframe->a0);
    }
  } else {
    // 如果系统调用号无效，打印错误信息
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
```

接下来就是`sys_trace`的具体实现，在`kernel/sysfile.c`中

```c
uint64 sys_trace(void)
{
  int mask;
  if(argint(0, &mask) < 0)//将传入的第几个参数赋给mask
    return -1;
  myproc()->mask = mask;
  return 0;
}
```

## sysinfo

简单介绍：sysinfo该系统调用的主要功能是获取系统中的可用内存页的大小和正在运行的进程数。

首先在`kernel/kalloc.c`中添加`freemem`函数来供`sys_sysinfo`调用

```c
uint64
freemem(void)
{
  struct run *cursor;
  uint64 freepage = 0;
  acquire(&kmem.lock);
  cursor = kmem.freelist;
  while (cursor)
  {
    freepage += PGSIZE;
    cursor = cursor->next;
  }
  release(&kmem.lock);
  return freepage;
}
```

同时也要在`kernel/proc.c`中添加`nproc`函数来供sys_sysinfo调用

```c
uint64
nproc(void)
{
  struct proc *p;
  uint64 count = 0;
  for(p = proc; p < &proc[NPROC]; p++)
  {
    if(p->state != UNUSED)
      count++;
  }
  return count;
}
```

注意要在`kernel/syscall.h`中添加对应宏

```c
#define SYS_sysinfo  23
```

然后在`kernel/syscall.c`中添加系统调用的接口，确保可在触发系统调用后成功调用

```c
extern uint64 sys_sysinfo(void);
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
...
[SYS_sysinfo] sys_sysinfo,
};
static char *syscall_names[] = {
  [SYS_fork]    "fork",
  [SYS_exit]    "exit",
  [SYS_wait]    "wait",
...
  [SYS_sysinfo] "sysinfo",
};
```

最后在`kernel/sysfile.c`中补充`uint64 sys_sysinfo(void);`的实现：

```c
uint64 sys_sysinfo(void)
{
  uint64 addr;
  struct sysinfo info;
  // 获取用户传入指针的地址，用户态中info是一个结构体指针，所以需要使用argaddr
  if(argaddr(0, &addr) < 0)
    return -1;
  info.freemem = freemem();
  info.nproc = nproc();
  // 使用copyout将info结构体复制到用户空间
  if(copyout(myproc()->pagetable, addr, (char*)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
```

## 总结

​	用户进程是通过`trap`发起系统调用，在发生系统调用之后，根据系统调用的调用号（保存在寄存器中），执行对应的函数指针。注意这里的函数指针都是没有接受参数的，这是因为在发生系统调用的时候，用户态所接受的参数都在中断处理程序中被保存在了寄存器之中，因此这里使用了`argaddr`，`argint`这类的函数来读取需要传入的函数，同时也使用了`copyout`函数将结果写会用户进程。
