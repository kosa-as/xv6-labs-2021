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
// 在开启cow之后，kfree不再直接释放页，而是通过引用计数来控制页的释放
// 如果引用计数大于1，则不释放页，而是减少引用计数
// 如果引用计数为0，则释放页
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

接下来实现 `cow_handler`函数，在 `kernel/vm.c`中添加如下代码

```c
int cow_handler(pagetable_t pagetable, uint64 va) // 发生COW缺页的处理函数
{
  if (va >= MAXVA) //必须要做越界检查，不然无法通过usertests
    return -1;
  pte_t *pte;
  pte = walk(pagetable, va, 0); //获取传入地址的页表项，判断能否正常寻找到物理页
  if (pte == 0) return -1;
  if ((*pte & PTE_U) == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_COW) == 0)//检查这个物理页的权限
    return -1;

  // allocate a new page
  uint64 pa = PTE2PA(*pte); // 获取原有物理页的地址
  uint64 ka = (uint64) kalloc(); // 分配一个新的物理页

  if (ka == 0){
    return -1; // out of memory
  }

  memmove((char*)ka, (char*)pa, PGSIZE); // 将原有物理页的内容复制到新的物理页中

  // 直接修改页表项，而不是使用mappages
  uint flags = PTE_FLAGS(*pte); // 获取原有的标志位
  *pte = PA2PTE(ka) | (flags & ~PTE_COW) | PTE_W; // 设置新的物理地址的pte，移除COW标记，添加写权限
  
  sfence_vma(); // flush TLB
  kfree((void*)pa); // 调用kfree，根据减少引用次数
  
  return 0;
}
```

最后，在内核态写回用户空间的时候，需要调用`copyout`函数来实现数据的传输。在 `kernel/vm.c`中修改`copyout`函数如下

```c
// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);//得到所访问页的起始虚拟地址
    
    // 检查虚拟地址是否在有效范围内
    if(va0 >= MAXVA) {
      return -1;
    }
    
    pa0 = walkaddr(pagetable, va0);//获取该页的物理地址,并判断是否有效
    if(pa0 == 0) {
      return -1;
    }
    
    pte_t* pte = walk(pagetable, va0, 0);//获取该页的页表项
    if (pte == 0  || (*pte & PTE_V) == 0) {
      return -1;
    }
    if (*pte & PTE_COW) { //写入的目标页是带有COW标记的页
      // 如果是COW页，分配一个新的物理页，并将原来的内容复制到新的物理页上。
      uint64 pa0_new;
      if((pa0_new = (uint64)kalloc()) == 0) {
        return -1;
      }
      memmove((void *)pa0_new, (void *)(pa0), PGSIZE);
      // 直接更新页表项，而不是使用mappages,这里使用mappages会
      // 设置旧的页表项映射到新的物理页地址，并移除COW标记，添加写权限
      *pte = PA2PTE(pa0_new) | (PTE_FLAGS(*pte) & ~PTE_COW) | PTE_W;
      sfence_vma(); // flush TLB for this page
      // 使用kfree来正确处理引用计数
      kfree((void*)pa0);
      pa0 = pa0_new;
    }
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
```

## 总结

- 在xv6中,通过了pagetable_t来管理其用户态虚拟地址空间,它指向sv39页表结构的最高层页表索引.
- xv6通过在页表项中添加COW标记位来实现Copy-on-Write技术
  - 在fork时,只复制父进程的页表结构,父进程下的所有页的标记位设置为可读和COW.
  - 当子进程或者父进程需要写入带有COW标记的页时,触发缺页异常,然后分配新的物理页并复制内容.当引用计数为0的时候,释放掉物理页.
- 在内核向用户空间写入数据时,使用copyout函数来处理COW页的写入,如果是COW页,则分配新的物理页并复制内容到新的物理页上,然后更新页表项.
