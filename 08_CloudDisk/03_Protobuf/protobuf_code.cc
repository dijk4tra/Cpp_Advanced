#include "person.pb.h"
#include <bitset> // 位图：位的数组
#include <iostream>
#include <string>

using namespace std;
using namespace test;

int main()
{
    Person p;
    p.set_id(100);
    p.set_name("aaa");

    string data;
    p.SerializeToString(&data); // 序列化

    cout << "size: " << data.size() << endl;
    for (const auto& c : data) {
        cout << bitset<8>(c) << " ";
    }
    cout << endl;
}
