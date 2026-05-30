#include <iostream>
#include <utfcpp/utf8.h>

using namespace std;

int main()
{
    string s = "你好，世界！!";

    const char* curr = s.c_str();
    const char* end = s.c_str() + s.size();

    while (curr != end) {
        auto start = curr;
        // 将it移动到下一个utf8字符所在的位置
        utf8::next(curr, end);
        // 一个汉字会占用多个字节，因此需要用string来表示一个汉字
        string character = string(start, curr);
        cout << character << endl;
    }
}
