#include <iostream>
#include <utfcpp/utf8.h>

using namespace std;
int main()
{
    string s = "你好，世界！!";
    // 获取utf8的起始迭代器和末尾迭代器
    auto it = utf8::iterator<string::const_iterator>(s.begin(), s.begin(), s.end());
    auto end = utf8::iterator<string::const_iterator>(s.end(), s.begin(), s.end());

    for (; it != end; ++it) {
        char32_t codepoint = *it; // 获取码点
        cout << "U+" << hex << codepoint << endl; // 以十六进制格式输出
    }
}
