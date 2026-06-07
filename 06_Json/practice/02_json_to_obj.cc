#include <iostream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

struct Book {
    string title;
    string author;
    double price;
    string publisher;
    vector<string> tags;
};

int main()
{
    // 原始 JSON 字符串（使用 C++11 的原始字符串字面量 R"(...)" 避免转义字符）
    string json_str = R"({
            "book": {
                "title": "JavaScript 高级程序设计",
                "author": "Nicholas C. Zakas",
                "price": 129.00,
                "publisher": "人民邮电出版社",
                "tags": ["前端", "JavaScript", "编程"]
            }
        })";


    // 1. 将字符串解析为 json 对象
    json j = json::parse(json_str);
    cout << j.dump(2) << endl;

    // 2. 创建并初始化Book对象
    Book book;
    book.title = j["book"]["title"];
    book.author = j["book"]["author"];
    book.price = j["book"]["price"];
    book.publisher = j["book"]["publisher"];
    book.tags = j["book"]["tags"];

    // 3. 打印书名、价格和标签
    cout << "title: " << book.title
         << ", price: " << book.price
         << ", tags: [";

    int ntags = book.tags.size();
    for (int i = 0; i < ntags; ++i) {
        if (i != ntags - 1) {
            cout << book.tags[i] << ", ";
        } else {
            cout << book.tags[i] << "]" << endl;
        }
    }

    return 0;
}
