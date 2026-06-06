#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

int main()
{
    // 1. 解析文件
    ifstream f("example.json");
    json j1 = json::parse(f); // 反序列化
    // cout << j1.dump() << endl; // 序列化
    cout << j1.dump(2) << endl; // 缩进单位为2个空格

    // 2. 解析字符串 (裸字符串: Raw String)
    json j2 = json::parse(R"({
        "pi": 3.14159,
        "happy": true
    })");
    cout << j2.dump(2) << endl;

}
