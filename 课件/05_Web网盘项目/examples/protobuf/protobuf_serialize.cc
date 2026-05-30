#include "Person.pb.h"
#include <fstream>
#include <iostream>
#include <string>

using namespace std;
using namespace test;

int main()
{
    Person p1;
    p1.set_name("test");
    p1.set_id(100);
    p1.set_email("example@gmail.com");

    // 序列化: 将C++中的结构体保存到二进制序列中
    std::string output;
    p1.SerializeToString(&output); // 序列化
    cout << "size: " << output.size() << endl;
    cout << "output: " << output << endl;

    // 反序列化
    Person p2;
    cout << "p2.name: " << p2.name()
         << ", p2.id: " << p2.id()
         << ", p2.email: " << p2.email() << endl;

    p2.ParseFromString(output); // 反序列化
    cout << "p2.name: " << p2.name()
         << ", p2.id: " << p2.id()
         << ", p2.email: " << p2.email() << endl;
}
