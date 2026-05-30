#include <ios>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

int main()
{
    // 1. 判断JSON的数据类型
    json data;
    data.is_null();
    data.is_number();
    data.is_boolean();
    data.is_string();
    data.is_array();
    data.is_object();
    // 2. 解析基本数据类型：直接赋值
    json j1 = 3.14; // number
    double pi = j1;
    cout << pi << endl; // 3.14
    json j2 = true; // boolean
    bool flag = j2;
    cout << boolalpha << flag << endl; // true
    json j3 = "茜茜";
    string name = j3;
    cout << name << endl; // "茜茜"
    // 3. nlohmann/json拥有getter方法，可以显示指定转换后的类型
    json d1 = 3.14; // number
    double pie = d1.get<double>();
    cout << pie << endl; // 3.14
    json d2 = true; // boolean
    bool ok = d2.get<bool>();
    cout << boolalpha << ok << endl; // true
    json d3 = "茜茜";
    string idol = d3.get<string>();
    cout << idol << endl; // "茜茜"
    // 4. 解析数组
    json array = { "peanut", "loves", "xixi", 520 };
    // 访问元素
    cout << array[0] << " "
         << array[1] << " "
         << array[2] << " "
         << array[3] << endl; // "peanut" "loves" "xixi" 520
    // 遍历数组
    for (auto& element : array) {
        cout << element << " ";
    }
    cout << endl;
    // 5. 解析对象
    json object = {
        { "id", 1 },
        { "name", "花生" },
        { "age", 18 }
    };
    // 根据键访问值
    cout << object["id"] << " "
         << object["name"] << " "
         << object["age"] << endl; // 1 "花生" 18
    // 遍历对象
    for (auto& entry : object.items()) {
        cout << entry.key() << ": " << entry.value() << endl;
    }
}
