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
