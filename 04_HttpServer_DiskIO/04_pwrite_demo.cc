#include "common.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>

int main()
{
    int fd = open("a.txt", O_RDWR);
    assert(fd != -1 && "open failed");

    char buf[4096] = {0};
    // 获取当前文件位置
    printf("pos: %ld\n", lseek(fd, 0, SEEK_CUR)); // pos: 0
    // write(fd, "kitty", 5);
    pwrite(fd, "kitty", 5, 6); // 从第7个字节开始写入
    // 区别: write会改变文件位置，pwrite不会
    printf("pos: %ld\n", lseek(fd, 0, SEEK_CUR)); // pos: 0
    printf("buf: %s\n", buf); // 从第7个字节开始读取
    close(fd);
}
