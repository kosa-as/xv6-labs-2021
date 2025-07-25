# Lab：Copy-on-Write

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

本实验由于需要修改内核代码之后，很可能导致 `qemu`无法正常启动，建议结合 `make qemu-debug`和 `gdb-multiarch`来排除问题

在 `make qemu`的时候，需要修改 `makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

## Copy-on-Write

简要介绍：众所周知，操作系统在调用 `fork`来创建子进程的时候，子进程会复制所有的父进程的内存空间。这里就出现了很严重的问题，如果全部都在物理页上复制一遍，那么开销是巨大的。因此必须要引入 `copy-on-write`技术，即只复制父进程的 `pagetable`，即只复制父进程的页表结构，不复制物理页。同时，将其中所有对应的页的标记置为可读，并添加上COW标签，用于后续处理冲突。当子或父进程需要写入用户页的时候，这时要触发缺页处理，即分配新的页表，同时修改页表项。通过这种方式，可以极大减少进程复制的开销。

首先添加必要的宏，在 `kernel/memlayout.h`中，添加一共有多少个物理页的宏

```c
#define MAX_PHYSICAL_PAGE_NUM (PHYSTOP >> PGSHIFT)
```

在 `kernel/riscv.h`中添加物理地址到引用计数数组索引的宏，和COW标记位的宏

```c
#define PA2REFIDX(pa) (pa  >> PGSHIFT)
#define PTE_COW (1L << 6) // copy on write
```

然后在 `kernel/kalloc.c`中添加引用计数数组，并且添加管理数组的自旋锁。同时，在 `kinit`中添加对 `ref_lock`的初始化，在 `freerange`中添加对引用计数的初始化。注意，这里使用 `memset`会出严重的问题，因为 `memset`是逐字节置为 `0x01`的

```c
int page_ref_count[MAX_PHYSICAL_PAGE_NUM];
struct spinlock ref_lock;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref_lock, "ref_count");
  // memset(page_ref_count, 1, MAX_PHYSICAL_PAGE_NUM * sizeof(int));
  freerange(end, (void*)PHYSTOP);
  printf("initialized kalloc, end = %p, PHYSTOP = %p\n", end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    // printf("freerange: p = %p\n", p);
    page_ref_count[PA2REFIDX((uint64)p)] = 1; // 初始化引用计数为1
    kfree(p);
  } 
}

```

同时，修改在 `kernel/kalloc.c`中的 `kalloc`和 `kfree`函数中对页的管理

```c
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int release_flag = 0;
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  // 检查引用计数，控制页的释放
  int index = PA2REFIDX((uint64)pa);
  acquire(&ref_lock);
  if(page_ref_count[index] < 1) {
    panic("kfree: page_ref_count < 1");
  }
  page_ref_count[index]--;
  release_flag = page_ref_count[index];
  release(&ref_lock);

  if(release_flag == 0){
  //只有当引用计数为0时才释放
    memset(pa, 1, PGSIZE);
    r = (struct run*)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
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
  if(r){//初始化获得的页
    kmem.freelist = r->next;
    acquire(&ref_lock);
    page_ref_count[PA2REFIDX((uint64)r)] = 1;
    release(&ref_lock);
  }
  release(&kmem.lock);
  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
  }
  return (void*)r;
}

void increase_ref_count(uint64 pa){
  if(pa >  PHYSTOP) //添加越界判断
    panic("increase_ref_count: pa out of range");
  acquire(&ref_lock);
  if(page_ref_count[PA2REFIDX(pa)] < 1) {
    panic("increase_ref_count: page_ref_count < 1");
  }
  page_ref_count[PA2REFIDX(pa)]++;
  release(&ref_lock);
}

```

在阅读 `kernel/proc.c`可以得知，在 原来的实现中， `fork`通过调用 `uvmcopy`来实现对页表结构的复制。因此首先去修改 `kernel/vm.c`中的 `uvmcopy`函数

```c

int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  for(i = 0; i < sz; i += PGSIZE){//针对所有的用户进程空间，每页都要遍历一次并且修改标记位
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);//获取页帧的物理地址
    increase_ref_count(pa);
    //移除父进程的写权限，同时将页打上COW标记
    *pte &= ~PTE_W;
    *pte |= PTE_COW;
    flags = PTE_FLAGS(*pte);
    // if((mem = kalloc()) == 0)
    //   goto err;
    // memmove(mem, (char*)pa, PGSIZE);
    //提取flag去映射子页表，注意这里并没给子进程分配新的物理页，而是直接映射父进程的物理页，并打上flag
    if(mappages(new, i, PGSIZE, pa, flags) != 0){
      // kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
```

然后去修改遇到缺页错误的处理，在 `kernel/trap.c`的 `usertrap`中作出如下修改

```c
...
if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if(r_scause() == 0xd || r_scause() == 0xf){// 处理加载或者存储的缺页错误
    if(cow_handler(p->pagetable, r_stval()) < 0){//调用cow处理函数
      printf("usertrap(): page fault at %p pid=%d\n", r_stval(), p->pid);
      p->killed = 1;
    }
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }
...
```

## 总结

xv6-2021fall 的页表实验实现了基于 RISC-V Sv39 格式的三级页表结构，涉及虚拟地址到物理地址的多级转换机制。通过 `walk` 实现页表项查找与可选分配，`mappages` 用于建立虚实映射，`uvmalloc` 完成用户内存空间扩展，`copyout` 和 `copyin` 实现内核与用户态的数据传输，`vmprint` 可视化页表结构。通过实现该部分，可以更好的理解操作系统中页表是怎么实际管理的
