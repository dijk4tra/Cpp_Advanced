#include <iostream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

using namespace std;
// 给 nlohmann::json 起一个别名方便使用
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

    // 手动映射并构建符合要求的 JSON 对象
    json j;
    j["id"] = s.id;
    j["name"] = s.name;

    // 嵌套构建 scores 对象
    j["scores"]["chinese"] = s.chinese;
    j["scores"]["math"] = s.math;
    j["scores"]["english"] = s.english;

    // 库会自动将 std::vector 转换为 JSON 数组
    j["hobbies"] = s.hobbies;
    j["isGraduated"] = s.isGraduated;

    // 序列化输出，使用 s.dump(2) 可以格式化输出（2个空格缩进）
    cout << j.dump(4) << endl;

    return 0;
}
