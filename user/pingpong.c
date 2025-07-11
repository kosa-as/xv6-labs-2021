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
