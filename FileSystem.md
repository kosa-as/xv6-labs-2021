# Lab：File System

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

本实验由于需要修改内核代码之后，很可能导致 `qemu`无法正常启动，建议结合 `make qemu-debug`和 `gdb-multiarch`来排除问题

在 `make qemu`的时候，需要修改 `makefile`的如下内容，保证make的时候不会报错, 同时添加 `$U/_symlinktest`来进行后续软链接的测试

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
...
...
ifeq ($(LAB),fs)
UPROGS += \
	$U/_bigfile\
	$U/_symlinktest
endif
```

## Large files

简要介绍：在xv6中，数据在磁盘上的存储是按照块的, 这里原本的file system只支持了inode的一级索引.因此只能分配十分有限的块.本部分所要做的就是为xv6添加二级间接块索引的方式,使得文件系统可以管理且提供更多的块给用户使用.(更多详细的内容可以参考教科书的磁盘块索引构建方式)

首先,修改 `kernel/fs.h`的dinode的定义,并且修改和添加宏.这里addrs数组存储的是块在存储设备上的地址

```c
#define NDIRECT 11 //添加一个二级间接块
#define NINDIRECT (BSIZE / sizeof(uint)) //每个间接块可以存储的地址数量
#define NINDIRECT2 (NINDIRECT * NINDIRECT) //每个二级间接块可以存储的地址数量
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT2) //最大文件大小（块数）

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+2];   // Data block addresses
};
```

同时,还要修改在 `kernel/file.h`中inode相关的定义

```c
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT+2];
};
```

接下来修改kernel/fs.c中的bmap函数,bmap函数负责的是,根据传入的块号和inode,来获取块在磁盘上真实的物理地址.阅读代码可以得知bmap在writei和readi中被使用,用于获取到实际写入块号的物理地址.

```c
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){ // 直接查找直接块
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){// 查找一级间接块
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;
  if(bn < NINDIRECT2){ // 处理二级间接块范围内的逻辑块
      int index1 = bn / NINDIRECT; // 在二级间接表中的一级索引
      int index2 = bn % NINDIRECT; // 在对应一级间接表中的二级索引

      if((addr = ip->addrs[NDIRECT + 1]) == 0) // 顶层二级间接块不存在，分配
          ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);

      bp = bread(ip->dev, addr);
      a = (uint*)bp->data;

      if((addr = a[index1]) == 0){ // 对应的一级间接块不存在，分配
          a[index1] = addr = balloc(ip->dev);
          log_write(bp);
      }
      brelse(bp);

      bp = bread(ip->dev, addr);
      a = (uint*)bp->data;

      if((addr = a[index2]) == 0){ // 对应的数据块不存在，分配
          a[index2] = addr = balloc(ip->dev);
          log_write(bp);
      }
      brelse(bp);
      return addr;
  }

  panic("bmap: out of range");
}
```

对应的，修改释放块的函数itrunc.和上面的逻辑相似,需要注意的是先释放二级最底层的数据块,才能释放上级的数据块

```c
// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  if(ip->addrs[NDIRECT + 1]) {
      // 读取二级间接块
      bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
      a = (uint*)bp->data;

      // 遍历二级间接块中的每个一级间接块指针
      for (i = 0; i < NINDIRECT; i++) {
          if (a[i]) {
              struct buf *bp1 = bread(ip->dev, a[i]);
              uint *a1 = (uint*)bp1->data;

              // 遍历一级间接块中的每个数据块指针
              for (j = 0; j < NINDIRECT; j++) {
                  if (a1[j]) {
                      bfree(ip->dev, a1[j]); // 释放数据块
                  }
              }
              brelse(bp1);
              bfree(ip->dev, a[i]); // 释放一级间接块
          }
      }
      brelse(bp);
      bfree(ip->dev, ip->addrs[NDIRECT + 1]); // 释放二级间接块
      ip->addrs[NDIRECT + 1] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
```

## Symlink

本部分实现的是xv6的软链接功能.首先介绍一下软链接和硬链接.

硬链接:硬链接就是给一个已有的文件  **创建一个新的目录项** ，多个目录项指向同一个  **inode** （即同一个物理文件数据）

软链接:软链接是一个特殊的文件，它的内容是“另一个文件的路径名”,类似于桌面的快捷方式

完成本部分首先要添加 `SYS_symlink`系统调用.这里不做赘述,详细怎么做参考系统调用部分的实验内容.

首先在 `kernel/sysfile.c`中添加 `symlink`的系统调用

```c
uint64
sys_symlink(void)
{
  char target[MAXPATH];
  char path[MAXPATH];
  struct inode *ip;
  int n;

  if((n = argstr(0, target, MAXPATH)) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();

  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0){
      end_op();
      return -1;
  }
  // ilock(ip);调用create之后,返回的是一个lock的ip,这里不能在加锁,加锁会导致后面卡死在spinlock的获取
  // create a new symlink, return with a locked inode
  int len = strlen(target);
  if(len < 0 || len >= MAXPATH -1){
    iunlockput(ip);
    end_op();
    return -1;
  }
  if(writei(ip, 0, (uint64)&len, 0, sizeof(len)) != sizeof(len)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(writei(ip, 0, (uint64)target, sizeof(len), len+1) != len+1){
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
  return 0;
}
```

同时修改打开文件的系统调用,在处理完 `omode == O_CREATE`的分支之后立即处理符号链接类型的文件

```c
...
if (ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)) {//保证本身不是软链接文件
    int tolerate = 10;
    int len = 0;
    char target[MAXPATH];
    while (ip->type == T_SYMLINK && tolerate > 0) {
      if (readi(ip, 0, (uint64)&len, 0, sizeof(len)) != sizeof(len)) {
        iunlockput(ip);
        end_op();
        return -1;
      }
      if (len <= 0 || len >= MAXPATH - 1) {
        iunlockput(ip);
        end_op();
        return -1;
      }
      if (readi(ip, 0, (uint64)target, sizeof(len), len+1) != len+1) {
        iunlockput(ip);
        end_op();
        return -1;
      }
      iunlockput(ip);
      if((ip = namei(target)) == 0) { //存在软链接的递归调用,获取下一个访问的inode
        end_op();
        return -1;
      }
      ilock(ip);
      tolerate--;
    }
    // cycle symlink is not allowed
    if (tolerate == 0) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }
...
```

## 总结

    要深入理解文件系统,要从虚拟文件系统的角度出发,看它是如何为上层的用户提供访问的接口,以及如何管理被挂载的设备驱动器.这些就涉及到了file,file_struct,fd,dentry,inode的概念,这些部分是如何配合的需要深入的了解学习.

而针对文件系统,数据是以块的形式存储在存储中的,而块的管理则是通过inode来实现

    在本实验中，我从虚拟文件系统（VFS）的角度加深了对文件系统的理解。VFS 通过`fd → file → inode` 的链路为用户提供统一的访问接口，并通过路径解析（目录项 → inode）来管理挂载的存储设备。

在 xv6 中，`struct file` 表示一次打开的文件，进程通过 `proc->ofile[]` 建立 `fd` 与 `file` 的映射；`struct inode` 则管理文件的元信息及数据块映射，实现逻辑块号到磁盘物理块号的转换。虽然 xv6 没有完整的 dentry 缓存机制，但其简化的设计清晰展现了文件系统的核心运行流程。

通过本实验，我理解了  **fd、file、inode 等结构之间的配合关系** ，以及文件如何通过 inode 组织和管理底层块存储，为进一步学习更复杂的 Linux VFS 打下了基础。
