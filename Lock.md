# Lab：Lock

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

本实验由于需要修改内核代码之后，很可能导致 `qemu`无法正常启动，建议结合 `make qemu-debug`和 `gdb-multiarch`来排除问题

在 `make qemu`的时候，需要修改 `makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

## Memory allocator

简要介绍：在xv6中，内存分配器是一个简单的基于页的分配器。它使用了一个全局的 `freelist`加上一个自旋锁来管理空闲页。在多线程的环境下，这样的效率很低，因此这里引入了Per-CPU的概念，每个CPU都有自己的空闲页链表和管理的自旋锁。这样可以减少原有设计下锁的竞争，提高性能。下面是修改的代码片段。

首先什么Per-CPU变量，这里为每个CPU都申请一个kmem_t：

```c
typedef struct {
  struct spinlock lock;
  struct run *freelist;
} kmem_t;

kmem_t kmemlist[NCPU];
```

修改初始化的部分，包括`kinit`和`freerange`。这里按照Lab要求将`freerange`函数将所有的空闲内存页分配给当前CPU的kmemlist。

```c
void
kinit()
{
  for(int i = 0; i < NCPU; i++) {
    initlock(&kmemlist[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{//Let freerange give all free memory to the CPU running freerange.
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  push_off();// 禁止中断，保证cpuid()返回正确的结果
  int cpu_id = cpuid();
  pop_off();
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    struct run *r = (struct run*)p;
    acquire(&kmemlist[cpu_id].lock);
    r->next = kmemlist[cpu_id].freelist;
    kmemlist[cpu_id].freelist = r;
    release(&kmemlist[cpu_id].lock);
  }
}
```

接下来是分配内存的函数`kalloc`，这里需要注意的是每个CPU都有自己的锁和空闲页链表。同时，每次申请优先从本链表上申请，如果当前CPU的空闲链表为空，则尝试从其他CPU的空闲链表中偷取一个页。但是其实博主博客写到这里才发现存在着死锁的风险。。。。。。。不想改了（bushi），要改很简单的就是略微修改一下代码，保证每次获取锁是按照顺序即可。

```c
void *
kalloc(void)
{
  struct run *r;
  push_off(); //关中断
  int cpu_id = cpuid();
  pop_off(); //开中断
  if(cpu_id < 0 || cpu_id >= NCPU)
    panic("kalloc: invalid CPU ID");
  acquire(&kmemlist[cpu_id].lock);
  r = kmemlist[cpu_id].freelist;
  if(r){
    kmemlist[cpu_id].freelist = r->next;
  }else{// 本CPU的空闲链表已经为空,去偷别的CPU的空闲链表
    int other_cpu_id;
    for(other_cpu_id = 0; other_cpu_id < NCPU; other_cpu_id++) {
      if(other_cpu_id == cpu_id) continue; 
      acquire(&kmemlist[other_cpu_id].lock);
      if(kmemlist[other_cpu_id].freelist) {
        r = kmemlist[other_cpu_id].freelist;
        kmemlist[other_cpu_id].freelist = r->next;
        release(&kmemlist[other_cpu_id].lock);
        break;
      }
      release(&kmemlist[other_cpu_id].lock);
    }
  }
  release(&kmemlist[cpu_id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
```

对应的，修改释放内存的函数`kfree`，这里需要注意的是每个CPU都有自己的锁和空闲页链表。

```c
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  push_off();//关中断
  int cpu_id = cpuid();
  pop_off();//开中断
  if(cpu_id < 0 || cpu_id >= NCPU)
    panic("kfree: invalid CPU ID");
  acquire(&kmemlist[cpu_id].lock);
  r->next = kmemlist[cpu_id].freelist;
  kmemlist[cpu_id].freelist = r;
  release(&kmemlist[cpu_id].lock);
}
```

## Buffer cache

简要介绍：在xv6中，缓冲区缓存（Buffer Cache）是一个用于存储磁盘块的缓存机制。原本使用的是一个全局的LRU机制的环状链表来管理，要求改为：使用一个哈希表来管理缓冲区，并且每个缓冲区都有一个锁来保证线程安全。使用多线程来提高缓冲区缓存的性能。

首先在`bcache`中定义一个用来存放`buf`的哈希桶，同时为每个桶添加一个自旋锁。这样可以保证每个桶的操作是线程安全的。

```c
#define HASHSIZE 23 // 哈希表的大小取质数
#define NULL ((void*)0) // 定义NULL指针

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct spinlock hashlock[HASHSIZE];
  struct buf buckets[HASHSIZE];
  // struct buf head;
} bcache;
```

为哈希表添加增加、删除和查找函数。这里的哈希函数使用了简单的模运算。

```c
static inline int hash(uint dev, uint blockno) {
  return (dev ^ blockno) % HASHSIZE;
}

static struct buf* find(uint dev, uint blockno) {
  int h = hash(dev, blockno);
  struct buf *b;
  for(b = bcache.buckets[h].next; b != NULL; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      return b;
    }
  }
  return NULL;
}

static void insert(struct buf *b){
  int h = hash(b->dev, b->blockno);
  b->next = bcache.buckets[h].next;
  bcache.buckets[h].next = b;
}

static void remove(struct buf *b) {
  int h = hash(b->dev, b->blockno);
  struct buf *cursor = &bcache.buckets[h];
  for(; cursor != NULL && cursor->next != NULL; cursor = cursor->next) { 
    // 这里要注意判断cursor->next是否为NULL,否则会出现越界行为
    if(cursor->next->dev == b->dev && cursor->next->blockno == b->blockno) {
      cursor->next = b->next;
      return;
    }
  }
}
```

同样的，为每个自旋锁添加初始化函数。并且初始化`buf`的睡眠锁。

```c
void
binit(void)
{
  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < HASHSIZE; i++) {
    initlock(&bcache.hashlock[i], "bchash");
  }
  for(int i = 0; i < NBUF; i++) {
    initsleeplock(&bcache.buf[i].lock, "buf");
  }

}
```

然后修改，获取缓冲区的函数`bget`。这个函数会查找是否已经存在对应的缓冲区，如果存在则增加引用计数并返回；如果不存在，则从空闲缓冲区中回收一个，并初始化它。注意这里的操作需要使用哈希表的锁来保证线程安全。同时，为了避免死锁，需要按照哈希值的顺序获取锁。

```c
// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int index = hash(dev, blockno);
  acquire(&bcache.hashlock[index]);

  // Is the block already cached?
  if((b = find(dev, blockno)) != NULL) {
    b->refcnt++;
    release(&bcache.hashlock[index]);
    acquiresleep(&b->lock);
    return b;
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(int i = 0; i < NBUF; i++) {
    if(bcache.buf[i].refcnt == 0) { // 找到一个未被引用的缓冲区
      int h = hash(bcache.buf[i].dev, bcache.buf[i].blockno);
      
      // 为了避免死锁，总是按照索引顺序获取锁
      if(h != index) {
        if(h < index) {
          release(&bcache.hashlock[index]);
          acquire(&bcache.hashlock[h]);
          acquire(&bcache.hashlock[index]);
        } else {
          acquire(&bcache.hashlock[h]);
        }
      }
      
      // 重新检查 refcnt，因为我们可能释放过锁
      if(bcache.buf[i].refcnt != 0) {
        if(h != index) {
          release(&bcache.hashlock[h]);
        }
        continue;
      }
      
      b = &bcache.buf[i];
      remove(b);
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->disk = 0;
      b->refcnt = 1;
      insert(b);

      if(h != index) {
        release(&bcache.hashlock[h]);
      }
      release(&bcache.hashlock[index]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.hashlock[index]);
  panic("bget: no buffers");
}
```

在修改获得的函数后，同样的释放缓冲区的函数`brelse`也要修改

```c
// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  int index = hash(b->dev, b->blockno);
  acquire(&bcache.hashlock[index]);
  if(b->refcnt > 0) b->refcnt--;
  release(&bcache.hashlock[index]);
}
```

最后需要修改`bpin`和`bunpin`函数，这两个函数用于增加和减少缓冲区的引用计数。这里同样需要按照哈希值的顺序获取锁，以避免死锁。

```c
void
bpin(struct buf *b) {
  int index = hash(b->dev, b->blockno);
  acquire(&bcache.hashlock[index]);
  // acquire(&bcache.lock);
  b->refcnt++;
  // release(&bcache.lock);
  release(&bcache.hashlock[index]);
}

void
bunpin(struct buf *b) {
  int index = hash(b->dev, b->blockno);
  acquire(&bcache.hashlock[index]);
  // acquire(&bcache.lock);
  b->refcnt--;
  // release(&bcache.lock);
  release(&bcache.hashlock[index]);
}
```

## 总结

本实验主要是修改xv6的内存分配器和缓冲区缓存机制。主要通过引入锁同步下的多线程方式来提高性能，并且来避免冲突。同时需要注意的是，在多线程环境下，获取锁的顺序非常重要，以避免死锁的发生。
