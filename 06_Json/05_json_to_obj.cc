#include <iostream>
#include <vector>
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

    Book book; // C++对象
    book.title = data["book"]["title"];
    book.author = data["book"]["author"];
    book.price = data["book"]["price"];
    book.publisher = data["book"]["publisher"];

    for (const auto& tag : data["book"]["tags"]) {
        book.tags.push_back(tag);
    }

    cout << "title: " << book.title << endl;
    cout << "author: " << book.author << endl;
    cout << "price: " << book.price << endl;
    cout << "publisher: " << book.publisher << endl;
    cout << "tags: ";
    for (const auto& tag : book.tags) {
        cout << tag << " ";
    }
    cout << endl;

    return 0;
}
