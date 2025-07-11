#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int pd) {
    int prime;
    if (read(pd, &prime, sizeof(int)) <= 0) {
        close(pd);
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
        close(p2[1]);
        sieve(p2[0]);
        close(p2[0]);
        exit(0);
    }else{
        int num;
        close(p2[0]);
        while (read(pd, &num, sizeof(int)) > 0) {
            if (num % prime != 0) {
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