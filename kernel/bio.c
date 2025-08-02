// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define HASHSIZE 23
#define NULL ((void*)0)

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
    if(bcache.buf[i].refcnt == 0) {
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

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

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


