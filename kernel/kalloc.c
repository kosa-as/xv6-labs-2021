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

struct run {
  struct run *next;
};

typedef struct {
  struct spinlock lock;
  struct run *freelist;
} kmem_t;

kmem_t kmemlist[NCPU];

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
  push_off();
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

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
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

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
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
