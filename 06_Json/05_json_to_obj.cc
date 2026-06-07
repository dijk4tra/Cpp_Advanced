#include <iostream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

// 裸字符串: 字符串字面值.
const char* str = R"({
    "book": {
        "title": "JavaScript 高级程序设计",
        "author": "Nicholas C. Zakas",
        "price": 129.00,
        "publisher": "人民邮电出版社",
        "tags": ["前端", "JavaScript", "编程"]
    }
})";

struct Book {
    string title;
    string author;
    double price;
    string publisher;
    vector<string> tags;
};

int main() {
    // 将json字符串反序列化
    json data = json::parse(str);
    cout << data.dump(2) << endl;

    // 创建并初始化Book对象
    Book book; // C++对象
    book.title = data["book"]["title"];
    book.author = data["book"]["author"];
    book.price = data["book"]["price"];
    book.publisher = data["book"]["publisher"];
    book.tags = data["book"]["tags"];

    // for (const auto& tag : data["book"]["tags"]) {
    //     book.tags.push_back(tag);
    // }

    // 打印书名、价格和标签
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

    // for (const auto& tag : book.tags) {
    //     cout << tag << " ";
    // }
    // cout << endl;

    return 0;
}
