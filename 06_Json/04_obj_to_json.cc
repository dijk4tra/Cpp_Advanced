#include <iostream>
#include <vector>
#include <nlohmann/json.hpp>

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

int main() {
    Student s = { 1001, "花生", 92, 95, 88, { "唱歌", "跳舞", "rap", "篮球" }, false };

    json data;
    data["id"] = s.id;
    data["name"] = s.name;
    data["scores"]["chinese"] = s.chinese;
    data["scores"]["math"] = s.math;
    data["scores"]["english"] = s.english;
    data["hobbies"] = s.hobbies;
    data["isGraduated"] = s.isGraduated;

    cout << data.dump(2) << endl;
}
