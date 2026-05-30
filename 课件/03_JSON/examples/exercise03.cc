#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace std;
using json = nlohmann::json;

struct Student {
    string name;
    int age;
    string gender;
    int chinese;
    int math;
    int english;
};

const char* jsonstring = R"(
 [
     {
         "name": "赵一",
         "age": 18,
         "gender": "男",
         "scores": [85, 92, 78]
     },
     {
         "name": "钱二",
         "age": 19,
         "gender": "女",
         "scores": [96, 88, 94]
     },
     {
         "name": "孙三",
         "age": 18,
         "gender": "男",
         "scores": [76, 81, 69]
     }
 ])";

int main()
{
    json data = json::parse(jsonstring);
    vector<Student> students;

    // 遍历json数组，解析每一个json对象
    int max_score = 0;
    for (const auto& j : data) {
        Student s;
        s.name = j["name"];
        s.age = j["age"];
        s.gender = j["gender"];
        s.chinese = j["scores"][0];
        s.math = j["scores"][1];
        s.english = j["scores"][2];

        int total_score = s.chinese + s.math + s.english;
        if (total_score > max_score) {
            max_score = total_score;
        }
        students.push_back(s);
    }

    // a. 计算每个学生的平均分
    for (const auto& s : students) {
        cout << s.name << "的平均分为："
             << (s.chinese + s.math + s.english) / 3 << endl;
    }

    // b. 找出所有18岁的学生，并打印他们的名字
    cout << "18岁的学生有：";
    for (const auto& s : students) {
        if (s.age == 18) {
            cout << s.name << " ";
        }
    }
    cout << endl;

    // c. 找出总分最高的学生，并打印他们的名字
    cout << "分数最高的学生有：";
    for (const auto& s : students) {
        int total_score = s.chinese + s.math + s.english;
        if (total_score == max_score) {
            cout << s.name << " ";
        }
    }
    cout << endl;
}
