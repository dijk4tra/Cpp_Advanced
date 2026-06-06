#include <iostream>
#include <list>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

int main()
{
    // 1. 基本数据结构: null, bool, number, string
    json j1 ;
    cout << j1.dump() << endl; // null
    json j2 = false;
    cout << j2.dump() << endl; // false
    json j3 = 33;
    cout << j3.dump() << endl; // 2.33
    json j4 = "文嘉";
    cout << j4.dump() << endl; // "文嘉"

    cout << endl;

    // 2. 初始化列表
    json j5 = {"peanut", "loves", "lili", 520};
    cout << j5.dump(2) << endl;

    json j6 = {
        { "name", "花生" },
        { "age", 18 }
    };
    cout << j6.dump(2) << endl;

    cout << endl;

    // 3. 解决歧义
    json j7; // null
    json j8 = ""; // "" 空字符串
    json j9 = json::array(); // [] 空数组
    cout << j9.dump(2) << endl;
    json j10 = json::object(); // {} 空对象
    cout << j10.dump(2) << endl;

    cout << endl;

    // 如何表示: [["currency", "USD"], ["value", 2.33]]
    json d1 = { { "currency", "USD" }, { "value", 2.33 } };
    cout << d1.dump(2) << endl; // 解析成对象

    cout << endl;

    json d2 = json::array({ { "currency", "USD" }, { "value", 2.33 } });
    cout << d2.dump(2) << endl; // 解析成数组

    cout << endl;

    // 4. 动态构建json
    json d4; // null
    d4["pi"] = 3.141; // 发生了类型转换: null --> object
    d4["happy"] = true;
    d4["name"] = "Niels";
    d4["nothing"] = nullptr;
    d4["answer"]["everything"] = 33;
    d4["list"] = { 1, 0, 2};
    d4["object"] = {{"currency", "USD"}, {"value", 2.33}};
    cout << d4.dump(2) << endl;

    cout << endl;

    json d5; // null
    d5["pi"] = 3.141; // 发生了类型转换: null --> object
    d5["happy"] = true;
    d5["name"] = "Niels";
    d5["nothing"] = nullptr;
    d5["answer"]["everything"] = 33;
    // 数组(类似于vector)
    d5["list"].push_back(1);
    d5["list"].push_back(0);
    d5["list"].push_back(2);
    // 对象(类似于map)
    d5["object"].emplace("currency", "USD");
    d5["object"].emplace("value", 2.33);
    cout << d5.dump(2) << endl;

    // 5. STL容器可以轻松转换成JSON的数组或对象
    // 数组: <-- array, vector, list, forward_list, deque, stack, queue
    // 数组: <-- set, unordered_set, multiset, unordered_multiset
    list<double>numbers ={3.141 ,2.728 ,1.414 ,1.681 ,0.577 };
    json d6 = numbers;
    cout << d6.dump(2) << endl; //[3.141,2.728,1.414,1.681,0.577]

    // 对象: <-- map, unordered_map, multimap, unordered_multimap
    map<string,string>couples = {
        {"刘强东","章泽天"},
        {"文章","马伊琍"},
        {"李小璐","贾乃亮"},
        {"马蓉","王宝强"}
    };
    json d7 = couples;
    cout << d7.dump(2) << endl; // {"刘强东":"章泽天","文章":"马伊琍","李小璐":"贾乃亮","马蓉":"王宝强"}

}
