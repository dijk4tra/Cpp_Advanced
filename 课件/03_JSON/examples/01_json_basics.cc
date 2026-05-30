// 序列化：将内存中的数据结构或对象转换为可存储或可传输的格式（特定格式的字节流）的过程。
// 反序列化：序列化的逆过程，将字节流恢复为内存中的数据结构或对象。
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

int main()
{
    // 1. 解析文件
    ifstream f("example.json");
    json j1 = json::parse(f);
    // cout << j.dump() << endl;
    cout << j1.dump(2) << endl; // 以漂亮格式输出，缩进单位为2个空格
    // 2. 解析字符串（裸字符串）
    json j2 = json::parse(R"({
        "pi": 3.141,
        "happy": true
    })");
    cout << j2.dump(2) << endl;
}
