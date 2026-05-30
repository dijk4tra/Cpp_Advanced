#include <fstream>
#include <iostream>
#include <list>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

int main()
{
    // 1. 基本数据结构：直接赋值
    json j1;
    cout << j1.dump() << endl; // null
    json j2 = 3.14159;
    cout << j2.dump() << endl; // 3.14159
    json j3 = false;
    cout << j3.dump() << endl; // false
    json j4 = "花生";
    cout << j4.dump() << endl; // "花生"
    // 2. nlomann/json 支持初始化列表{...}
    json j5 = { "peanut", "loves", "xixi", 520 }; // 数组
    cout << j5.dump() << endl; // ["peanut","loves","xixi",520]
    json j6 = { // 对象
        { "happy", true },
        { "pi", 3.141 }
    };
    cout << j6.dump() << endl; // {"happy":true,"pi":3.141}
    // 3. 解决歧义
    // Q1: 如何表示空数组和空对象？
    json j7 = {};
    cout << j7.dump() << endl; // null
    json j9 = json::array();
    cout << j9.dump() << endl; // []
    json j10 = json::object();
    cout << j10.dump() << endl; // {}
    // Q2: 如何表示: [["currency", "USD"], ["value", 42.99]]
    json d1 = { { "currency", "USD" }, { "value", 42.99 } };
    cout << d1.dump() << endl; // { "currency": "USD", "value": 42.99 }
    // 遇到歧义的时候，我们需要显示指明是数组还是对象
    json d2 = json::array({ { "currency", "USD" }, { "value", 42.99 } });
    cout << d2.dump() << endl; // [["currency", "USD"], ["value", 42.99]]
    json d3 = json::object({ { "currency", "USD" }, { "value", 42.99 } });
    cout << d3.dump() << endl; // {"currency": "USD", "value": 42.99}
    // 4. 动态构建(适用于json中的数据来自多个不同数据结构的场景)
    json d4;
    d4["pi"] = 3.141;
    d4["happy"] = true;
    d4["name"] = "Niels";
    d4["nothing"] = nullptr;
    d4["answer"]["everything"] = 42;
    d4["list"] = { 1, 0, 2 };
    d4["object"] = { { "currency", "USD" }, { "value", 42.99 } };
    cout << d4.dump(2) << endl;
    json d5;
    d5["pi"] = 3.141;
    d5["happy"] = true;
    d5["name"] = "Niels";
    d5["nothing"] = nullptr;
    d5["answer"]["everything"] = 42;
    // 向数组中添加元素
    d5["list"].push_back(1);
    d5["list"].push_back(0);
    d5["list"].push_back(2);
    // 向对象中添加键值对
    d5["object"].emplace("currency", "USD");
    d5["object"].emplace("value", 42.9);
    cout << d5.dump(2) << endl;
    // 5. STL容器可以轻松地转换成JSON的数组或对象
    // 数组 <-- array, vector, deque, list, forward_list, stack, queue
    // 数组 <-- set, multiset, unordered_set,unordered_multiset
    list<double> numbers = { 3.141, 2.728, 1.414, 1.681, 0.577 };
    json d6 = numbers;
    cout << d6.dump() << endl; // [3.141, 2.728, 1.414, 1.681, 0.577]
    // 对象 <-- map, unordered_map, multimap, unordered_multimap
    map<string, string> couples = {
        { "刘强东", "章泽天" },
        { "文章", "马伊琍" },
        { "李小璐", "贾乃亮" },
        { "马蓉", "王宝强" }
    };
    json d7 = couples;
    cout << d7.dump() << endl;
}
