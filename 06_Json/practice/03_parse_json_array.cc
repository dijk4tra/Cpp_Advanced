#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;

int main() {
    // 原始 JSON 数据
    string data = R"([
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

    // 解析 JSON
    json students = json::parse(data);

    double max_total = -1;
    string top_student;

    cout << "每个学生的平均分：" << endl;
    for (auto& stu : students) {
        string name = stu["name"];
        vector<int> scores = stu["scores"];
        double sum = 0;
        for (int s : scores){
            sum += s;
        }
        double avg = sum / scores.size();
        cout << name << ": " << avg << endl;

        // 更新总分最高学生
        if (sum > max_total) {
            max_total = sum;
            top_student = name;
        }
    }

    // 找出所有 18 岁学生
    cout << "\n18岁学生名单：" << endl;
    for (auto& stu : students) {
        if (stu["age"] == 18) {
            cout << stu["name"] << endl;
        }
    }

    // 总分最高学生
    cout << "\n总分最高的学生：" << top_student << endl;

    return 0;
}
