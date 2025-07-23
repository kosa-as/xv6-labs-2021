// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
int page_ref_count[MAX_PHYSICAL_PAGE_NUM];
struct spinlock ref_lock;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref_lock, "ref_count");
  memset(page_ref_count, 1, MAX_PHYSICAL_PAGE_NUM * sizeof(int));
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    // printf("freerange: p = %p\n", p);
    kfree(p);
  } 
}

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

  if(release_flag == 0) {

    // Fill with junk to catch dangling refs.
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
  if(r){
    if(page_ref_count[PA2REFIDX((uint64)r)] < 1)
      panic("kalloc: page_ref_count < 1");
    kmem.freelist = r->next;
    page_ref_count[PA2REFIDX((uint64)r)] = 1;
  }
  release(&kmem.lock);
  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
  }
  return (void*)r;
}

void increase_ref_count(uint64 pa){
  if(pa >  PHYSTOP || pa < KERNBASE)
    panic("increase_ref_count: pa out of range");
  acquire(&ref_lock);
  if(page_ref_count[PA2REFIDX(pa)] < 1) {
    panic("increase_ref_count: page_ref_count < 1");
  }
  page_ref_count[PA2REFIDX(pa)]++;
  release(&ref_lock);
}

void get_ref_count(uint64 pa, int *count){
  if(pa >  PHYSTOP || pa < KERNBASE)
    panic("get_ref_count: pa out of range");
  acquire(&ref_lock);
  *count = page_ref_count[PA2REFIDX(pa)];
  release(&ref_lock);
}
