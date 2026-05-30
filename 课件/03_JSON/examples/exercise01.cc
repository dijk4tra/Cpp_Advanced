#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace std;
using json = nlohmann::json;

struct Student {
    int id;
    string name;
    int chinese;
    int math;
    int english;
    vector<string> hobbies;
    bool isGraduated;
};

int main()
{
    Student s = { 1001, "花生", 92, 95, 88, { "唱歌", "跳舞", "rap", "篮球" }, false };
    // 构建json对象
    json j;
    j["id"] = s.id;
    j["name"] = s.name;
    j["scores"]["chinese"] = s.chinese;
    j["scores"]["math"] = s.math;
    j["scores"]["english"] = s.english;
    j["hobbies"] = s.hobbies;
    j["isGraduated"] = s.isGraduated;
    // 以漂亮格式输出
    cout << j.dump(2) << endl;
}
