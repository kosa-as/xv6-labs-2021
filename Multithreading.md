# Lab：Multithreading

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

本实验由于需要修改内核代码之后，很可能导致 `qemu`无法正常启动，建议结合 `make qemu-debug`和 `gdb-multiarch`来排除问题

在 `make qemu`的时候，需要修改 `makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

## Uthread: switching between threads

简要介绍：在这个任务中，要去实现一个简单的用户态下的线程库。等待补充的是 `thread_create`和 `thread_schedule`部分。以及补充 `uthread_switch`中上下文切换针对上下文的保存。本部分针对代码的修改集中在 `user/uthread.c`和 `user/uthread_switch.S`中

首先定义上下文的数据结构：

```c
typedef struct context {
  uint64 ra;        /* return address */
  uint64 sp;        /* stack pointer */
  uint64 s[12];     /* callee-saved registers s0-s11 */
} context_t;
```

将上下文添加到 `struct thread`中：

```c
struct thread {
  char       stack[STACK_SIZE]; /* the thread's stack */
  int        state;             /* FREE, RUNNING, RUNNABLE */
  context_t  context;           /* 进程的上下文 */
};
```

在 `thread_create`同添加对返回地址和栈指针的保存

```c
void 
thread_create(void (*func)())
{
  struct thread *t;
  // 找到可以运行的线程描述符
  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;
  // YOUR CODE HERE
  t->context.ra = (uint64)func;  // 设置返回地址为函数入口
  t->context.sp = (uint64)t->stack + STACK_SIZE; // 设置栈指针+栈大小为栈顶，因为栈是向下增长的
}
```

然后在 `thread_schedule`中添加进程切换的上下文保存部分。注意这里不用设置被切换出去进程的状态，在 `thread_yield`中的具体实现可以明白

```c
void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  t = current_thread + 1;
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD)
      t = all_thread;
    if(t->state == RUNNABLE) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
    //切换线程
    thread_switch((uint64)&t->context, (uint64)&next_thread->context);
  } else
    next_thread = 0;
}
```

最后，添加上下文切换的寄存器保存实现的汇编代码：

```assembly
	.text

	/*
         * save the old thread's registers,
         * restore the new thread's registers.
         */

	.globl thread_switch
thread_switch:
	sd ra, 0(a0)
	sd sp, 8(a0)
	sd s0, 16(a0)
	sd s1, 24(a0)
	sd s2, 32(a0)
	sd s3, 40(a0)
	sd s4, 48(a0)
	sd s5, 56(a0)
	sd s6, 64(a0)
	sd s7, 72(a0)
	sd s8, 80(a0)
	sd s9, 88(a0)
	sd s10, 96(a0)
	sd s11, 104(a0)

	ld ra, 0(a1)
	ld sp, 8(a1)
	ld s0, 16(a1)
	ld s1, 24(a1)
	ld s2, 32(a1)
	ld s3, 40(a1)
	ld s4, 48(a1)
	ld s5, 56(a1)
	ld s6, 64(a1)
	ld s7, 72(a1)
	ld s8, 80(a1)
	ld s9, 88(a1)
	ld s10, 96(a1)
	ld s11, 104(a1)
	ret    /* return to ra */
```

## Using threads

简要介绍：在本部分中，主要实现的是多线程访问哈希表的实现。本实验中避免哈希冲突的解决方法是十字链表法。代码在 `notxv6/ph.c`中修改

首先给每个桶分配一个互斥锁

```c
pthread_mutex_t *mutexes[NBUCKET];
```

注意在 `main`函数中添加资源的申请和释放

```c
...  
for (int i = 0; i < NBUCKET; i++) {
    mutexes[i] = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(mutexes[i], NULL);
  }
...
...
  for (int i = 0; i < NBUCKET; i++) {
    pthread_mutex_destroy(mutexes[i]);
    free(mutexes[i]);
  }
...
```

这里主要修改 `put`函数

```c
static 
void put(int key, int value)
{
  int i = key % NBUCKET;
  struct entry *last_e = 0;
  // is the key already present?
  struct entry *e = 0;
  
  for (e = table[i]; e != 0; e = e->next) {//遍历阶段只读不写，可以并行执行
    if (e->key == key){
      break;
    }
    last_e = e;
  }
  if(e){//每到结尾就找到冲突，上锁更新
    // update the existing key.
    pthread_mutex_lock(mutexes[i]);
    e->value = value;
    pthread_mutex_unlock(mutexes[i]);
  } else {//到结尾还没找到冲突。注意这里可能后面新插入了，因此保存了last_e在遍历一次。这次遍历要上锁，保证不冲突
    pthread_mutex_lock(mutexes[i]);
    // the new is new.
    for(e = last_e; e != 0; e = e -> next){
      if (e->key == key){
        break;
      }
    }
    if(e){//同时发生了新插入的就更新
      e->value = value;
    }else{//没有新插入的就插入
      insert(key, value, &table[i], table[i]);
    }
    pthread_mutex_unlock(mutexes[i]);
  }
}
```

## Barrier

简要介绍：本部分主要是要完成同步的障碍，使用到了条件变量和互斥锁的结合/修改在 `xv6/barrier.c`中

```c
static void 
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
  pthread_mutex_lock(&bstate.barrier_mutex);
  bstate.nthread++;
  if (bstate.nthread == nthread) {
    bstate.round++;
    bstate.nthread = 0;
    pthread_cond_broadcast(&bstate.barrier_cond);
  } else {
    // while(bstate.nthread < nthread) { //suprious wake?
      pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
    // }
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```

## 总结

多线程编程在操作系统的层面，要注意的是上下文切换的寄存器数值保存，以及返回地址和新的栈指针的更新。而在整体的编程层面，需要注意的是互斥锁，自旋锁和条件变量的使用来保证同步。
