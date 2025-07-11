# Lab：Utils

@author ：[kosa-as](https://kosa-as.github.io/)

## 写在前面

在`make qemu`的时候，需要修改`makefile`的如下内容，保证make的时候不会报错

```makefile
#CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
```

## sleep

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(2, "Usage: sleep <seconds>\n");
    exit(1);
  }
  int n = atoi(argv[1]);
  sleep(n);
  exit(0);
}
```

直接调用sleep系统调用接口即可

## Pingpong

创建两个管道，用来保证父子进程的通信。需要注意的是，在使用管道的时候，如果需要读数据，就关闭管道的写端；写也是这样。

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int p1[2]; // 父进程到子进程的管道
    int p2[2]; // 子进程到父进程的管道
    char buf[1];
    int pid;

    // 创建两个管道
    if(pipe(p1) < 0 || pipe(p2) < 0){
        fprintf(2, "pipe failed\n");
        exit(1);
    }

    pid = fork();
    if(pid < 0){
        fprintf(2, "fork failed\n");
        exit(1);
    }

    if(pid == 0){
        // 子进程
        close(p1[1]); // 关闭父到子管道的写端
        close(p2[0]); // 关闭子到父管道的读端

        // 从父进程读取字节
        if(read(p1[0], buf, 1) != 1){
            fprintf(2, "child read failed\n");
            exit(1);
        }

        // 打印接收到ping的消息
        printf("%d: received ping\n", getpid());

        // 向父进程写入字节
        if(write(p2[1], buf, 1) != 1){
            fprintf(2, "child write failed\n");
            exit(1);
        }

        close(p1[0]);
        close(p2[1]);
        exit(0);
    } else {
        // 父进程
        close(p1[0]); // 关闭父到子管道的读端
        close(p2[1]); // 关闭子到父管道的写端

        // 向子进程发送字节
        buf[0] = 'a'; // 发送字符'a'
        if(write(p1[1], buf, 1) != 1){
            fprintf(2, "parent write failed\n");
            exit(1);
        }

        // 从子进程读取字节
        if(read(p2[0], buf, 1) != 1){
            fprintf(2, "parent read failed\n");
            exit(1);
        }

        // 打印接收到pong的消息
        printf("%d: received pong\n", getpid());

        close(p1[1]);
        close(p2[0]);
        wait(0); // 等待子进程结束
    }

    exit(0);
}
```

## primes

主要利用了`fork`和`pipe`实现了一个素数筛，具体的实现内容可以看代码注释

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int pd) {
    int prime;
    if (read(pd, &prime, sizeof(int)) <= 0) {//read返回的是读到数据的count
        close(pd); //main函数传入的管道读最终一定要关闭，当读不到的时候就关闭
        exit(0);
    }
    printf("prime %d\n", prime);
    int p2[2];
    if (pipe(p2) < 0) {
        fprintf(2, "pipe failed\n");
        exit(1);
    }
    int pid = fork();
    if (pid < 0) {
        fprintf(2, "fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        close(p2[1]);//关闭写
        sieve(p2[0]);//传递管道的读端口到下一个筛选的递归函数
        close(p2[0]);//关闭读
        exit(0);
    }else{
        int num;
        close(p2[0]);
        while (read(pd, &num, sizeof(int)) > 0) {
            if (num % prime != 0) {//如果除不尽，就发送到管道中
                write(p2[1], &num, sizeof(int));
            }
        }
        close(p2[1]);
        wait(0);
        exit(0);
    }
}


int main(int argc, char *argv[]) {

    int p1[2];
    if (pipe(p1) < 0) {
        fprintf(2, "pipe failed\n");
        exit(1);
    }

    int pid = fork();
    if (pid < 0) {
        fprintf(2, "fork failed\n");
        exit(1);
    }
    if (pid == 0) {//子进程
        close(p1[1]);//关闭写端
        sieve(p1[0]);//筛法
        close(p1[0]);//关闭读端
        exit(0);
    }else{
        close(p1[0]);//关闭读端
        for (int i = 2; i <= 35; i++) {
            write(p1[1], &i, sizeof(int));//写入管道
        }
        close(p1[1]);//关闭写端
        wait(0);
        exit(0);
    }
    exit(0);
}
```

## find

`struct dirent de;`是描述文件目录项目的数据结构；`struct stat st;`是描述文件具体属性的数据结构。了解这两个数据结构的内容，结合`user/ls.c`的内容，可以给出`find`的实现

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void
find(char *path, char *filename){
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_FILE:
    fprintf(2, "find: %s is a file\n", path);
    exit(1);

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("find: cannot stat %s\n", buf);
        continue;
      }
      if(st.type == T_DIR) {
        find(buf, filename);
      }else if(st.type == T_FILE) {
        if(strcmp(de.name, filename) == 0) {
          printf("%s\n", buf);
        }
      }
    }
    break;
  }
  close(fd);
}

int main(int argc, char *argv[])
{

  if(argc != 3){
    fprintf(2, "Usage: find <path> <filename>\n");
    exit(0);
  }

  find(argv[1], argv[2]);
  exit(0);
}

```

## xargs

了解一下exec的机制，和注意父进程必须等待子进程结束即可

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"


#define MAXLINE 512

int main(int argc, char *argv[]) {
    char line[MAXLINE];
    char *args[MAXARG];
    int line_pos = 0;
    int argc_base;
    int i;
    char c;
    
    if (argc < 2) {
        fprintf(2, "Usage: xargs <command> [args...]\n");
        exit(1);
    }
    
    // 复制基础命令和参数
    argc_base = argc - 1;
    for (i = 0; i < argc_base; i++) {
        args[i] = argv[i + 1];
    }
    
    // 逐字符读取标准输入
    while (read(0, &c, 1) == 1) {
        if (c == '\n') {
            // 遇到换行符，处理这一行
            line[line_pos] = '\0';  // null终止字符串
            
            // 将这一行作为额外参数添加
            args[argc_base] = line;
            args[argc_base + 1] = 0;  // null终止参数数组
            
            // fork和exec
            int pid = fork();
            if (pid == 0) {
                // 子进程
                exec(args[0], args);
                fprintf(2, "xargs: exec %s failed\n", args[0]);
                exit(1);
            } else if (pid > 0) {
                // 父进程
                wait(0);
            } else {
                fprintf(2, "xargs: fork failed\n");
                exit(1);
            }
            
            line_pos = 0;  // 重置行位置
        } else {
            // 普通字符，添加到当前行
            if (line_pos < MAXLINE - 1) {
                line[line_pos++] = c;
            }
        }
    }
    
    // 如果最后一行没有换行符，也要处理
    if (line_pos > 0) {
        line[line_pos] = '\0';
        args[argc_base] = line;
        args[argc_base + 1] = 0;
        
        int pid = fork();
        if (pid == 0) {
            exec(args[0], args);
            fprintf(2, "xargs: exec %s failed\n", args[0]);
            exit(1);
        } else if (pid > 0) {
            wait(0);
        } else {
            fprintf(2, "xargs: fork failed\n");
            exit(1);
        }
    }
    
    exit(0);
}

```

