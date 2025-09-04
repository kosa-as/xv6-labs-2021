# Lab：Traps

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

在`make qemu`的时候，需要修改`makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

## RISC-V assembly

简要介绍：在这个部分中，主要是阅读`make fs.img`之后产生的`user/call.asm`，通过阅读汇编代码来回答问题

1. Which registers contain arguments to functions?
在RISC-V架构中，函数参数通过寄存器a0到a7传递。

2. Where is the call to function f in the assembly code for main? Where is the call to g?
查找 'jal f' 或 'call f' 指令来定位对函数 f 的调用。但是在文中给出的例子，并没有发生函数的调用，这是因为编译器优化，直接将简单的f和g优化为内联函数了

3. At what address is the function printf located?
0x 0000000000000616 <printf>:

4. What value is in the register ra just after the jalr to printf in main?
    30:	00000097          	auipc	ra,0x0
    34:	5e6080e7          	jalr	1510(ra) # 616 <printf>
    可以得知，返回地址是  0x 38（jalr的作用是跳转并保存返回地址）

5. What is the output of the following code?
unsigned int i = 0x00646c72;
printf("H%x Wo%s", 57616, &i);
57616 in hex is e110 → output: He110
Little-endian layout of i: 0x72 ('r'), 0x6c ('l'), 0x64 ('d'), 0x00
So &i is "rld"
Final output: He110 World

6. If the RISC-V were big-endian, what value would you set i to yield the same output?
i = 0x726c64


7. What is going to be printed after 'y=' in this code? Why?
    printf("x=%d y=%d", 3);

  - This results in undefined behavior.

  - The format string expects two integers, but only one is provided.

  - x=3 y=<garbage_value> (whatever is in a1)
    这一段可以阅读`user/printf.c`代码可知。printf的核心实现vprintf，使用一个for循环从字符串fmt中从可变参数列表va_list中读取参数，通过宏va_arg来获取参数。
    识别到几个%就会调用几次。因此会发生UB问题，第二个的输出应该为不确定的值

## Backtrace

简要介绍：在触发`sys_sleep`系统调用之后，执行一次`backtrace`来查看函数调用的触发者地址。

在上一个部分中，阅读汇编代码可知。根据函数调用的原则，每次发生函数调用，一开始更新栈顶指针，在将函数的返回地址压入栈中，然后将调用者的栈底压入栈中，然后在更新栈底指针到最新的栈顶指针。

```assembly
  1c:	1141                	addi	sp,sp,-16
  1e:	e406                	sd	ra,8(sp)
  20:	e022                	sd	s0,0(sp)
  22:	0800                	addi	s0,sp,16
```

首先，在`kernel/def.h`中添加函数的声明

```c
void            backtrace(void);
```

然后根据实验要求，在`sys_sleep`的实现后面加上`backtrace`的调用

```c
uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  backtrace();
  return 0;
}
```

在`kernel/riscv.h`中添加对寄存器`s0`的访问，来读到栈底指针

```c
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, fp" : "=r" (x) );
  return x;
}
```

在`kernel/printf.c`中添加`backtrace`的实现

```c
void backtrace(void){
//xv6 内核中每个内核线程的栈大小是一页（4KB），起始地址是页对齐的。
//PGROUNDUP(fp) 是当前帧指针所在页的顶部地址（也就是栈的“底”），用于防止越过当前线程的栈范围。
  uint64 fp = r_fp();
  uint64 stack_bottom = PGROUNDUP(fp);
  printf("backtrace:\n");
  while (fp < stack_bottom) {
    printf("%p\n", *(uint64*)(fp - 8));//栈帧的返回地址
    fp = *(uint64*)(fp - 16);//调用者的栈帧
  }
}
```

## Alarm

简要介绍：在这个部分中，进程在执行时，当发出时钟周期到期的trap的时候，进行检查：如果已经经过指定的时钟周期数，调用`handler`来执行，执行完之后切换回原来的进程继续执行；否则继续执行。值得注意的是，这里需要处理上下文切换的数据保存。

建议：在实现这个部分前先去阅读`kernel/trap.c`的代码

首先在`user/user.h`中添加函数申明：

```c
#ifdef LAB_TRAPS
int sigalarm(int, void (*)(void));
int sigreturn(void);
#endif
```

然后在`user/usys.pl`中添加系统调用入口

```perl
entry("sigalarm");
entry("sigreturn");
```

记得添加在`kernel/syscall.h`中添加系统调用号

```c
#define SYS_sigalarm  22
#define SYS_sigreturn  23
```

以及补充在`kernel/syscall.c`的系统调用函数指针数组的补充

```c
#ifdef LAB_TRAPS
[SYS_sigalarm]   sys_sigalarm,
[SYS_sigreturn]   sys_sigreturn,
#endif
```

修改`kernel/proc.h`，补充所需要的字段

```c
struct proc{
  ...
  int sigalarm_interval;//系统调用中设定的经过时钟周期数
  uint64 sigalarm_handler;//系统调用中传入的函数指针
  uint64 sigalarm_ticks;//进程全局经过的时钟周期数
  struct trapframe *saved_trapframe;//保存切换到handler执行的原来的trapframe
  int is_running;//
  ...
}
```

在`kernel/proc.c`中补充这些字段的初始化和释放

在`allocproc`中修改，主要是进行页表的初始化，用`kalloc`分配内存

```c
...
found:
  p->pid = allocpid();
  p->state = USED;
  p->sigalarm_interval = 0;
  p->sigalarm_handler = 0;
  p->sigalarm_ticks = 0;
  p->is_running = 0;
  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  if((p->saved_trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
...
```

在`freeproc`中修改，进行资源的回收

```c
...  
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  if(p->saved_trapframe)
    kfree((void*)p->saved_trapframe);
  p->saved_trapframe = 0;
  p->sigalarm_interval = 0;
  p->sigalarm_handler = 0;
  p->sigalarm_ticks = 0;
  p->is_running = 0;
...
```

注意，这里不用将我们申请的页`saved_trapframe`同当前进程的用户空间地址映射，只是起到一个暂存的作用

然后在`kernel/sysproc.c`中实现`sys_sigalarm`和`sys_sigreturn`的实现

```c
uint64
sys_sigalarm(void){
  int interval;
  uint64 handler;
  if(argint(0, &interval) < 0)
    return -1;
  if(argaddr(1, &handler) < 0)
    return -1;
  myproc()->sigalarm_interval = interval;
  myproc()->sigalarm_handler = handler;
  myproc()->sigalarm_ticks = 0;
  return 0;
}

uint64
sys_sigreturn(void){
  struct proc *p = myproc();
  if(p->is_running == 0)
    return -1;
  p->is_running = 0;
  memcpy(p->trapframe, p->saved_trapframe, sizeof(struct trapframe));
  return 0;
}
```

最后在`trap.c`中`usertrap`函数中添加以下部分

```c
...
else if((which_dev = devintr()) ！= 0){
    //devintr() = 1是时钟中断，devintr() = 0则是内部中断，如缺页
    struct proc *p = myproc();
    if(p->sigalarm_interval && p->is_running == 0){//如果sigalarm_interval不为0,且没有陷入alarm系统调用
      if(p->sigalarm_interval == p->sigalarm_ticks){//进程已经执行达到了发出alarm的时钟周期数
        p->sigalarm_ticks = 0;
        p->is_running = 1;
        memcpy(p->saved_trapframe, p->trapframe, sizeof(struct trapframe));//保存上下文
        p->trapframe->epc = p->sigalarm_handler;//将handler绑定到下一个执行的地址，和上一步模拟上下文切换
      }
      p->sigalarm_ticks++;
    }
  } 
...
```

## 总结

在这个 `lab` 中，我对函数调用、系统调用和中断处理机制有了更深入的理解。

函数调用的主要步骤包括：

- 调用方使用 `call` 指令将返回地址压入栈；
- 被调用函数入口保存调用者保存的寄存器（如 `s0/fp`）形成新的栈帧；
- 更新栈顶指针，使其指向当前函数的局部变量空间；
- 函数返回时，恢复原寄存器并弹出返回地址，跳转回调用点。

与此不同，**系统调用本质上是一次由用户态发起的软中断（`ecall`）**，会导致控制权从用户空间转移到内核态。在中断处理过程中，关键步骤包括：

- 将用户态的寄存器值保存到当前进程的 `trapframe` 中；
- 切换到内核页表，跳转到中断处理函数；
- 执行对应的系统调用逻辑；
- 最后通过 `usertrapret` 恢复 trap 前的状态，使进程继续执行未完成的用户程序。

理解了整个保存现场 → 切换上下文 → 执行内核逻辑 → 恢复现场的过程，就掌握了中断机制的核心原理，也为深入理解操作系统的中断、系统调用与进程调度打下了基础。
