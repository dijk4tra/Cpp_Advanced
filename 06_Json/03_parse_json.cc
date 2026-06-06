#include <ios>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

int main()
{
    // 1．判断JSON的数据类型
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

    json j3 = "西西";
    string name = j3;
    cout << name << endl; // 西西

    // 3 .nlohmann/json拥有getter方法，可以显示指定转换后的类型
    json d1 = 3.14; // number
    double pie = j1;
    cout << pie << endl; // 3.14

    json d2 = true; // boolean



}
