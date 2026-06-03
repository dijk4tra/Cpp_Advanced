// 运行时断言:
//    判断表达式是否为真
//       真: 程序继续执行
//       假: 程序终止
// 静态断言:
#include <assert.h>
#include <stdio.h>

int main() {
    int a = 5;
    // Q: 如何给定出错信息(文本)?
    // 运行时断言
    assert(a == 4 && "a不等于5");

    // 编译时判断表达式的值
    static_assert(sizeof(int) == 4, "int的大小不为4");

    printf("main end\n");
}
