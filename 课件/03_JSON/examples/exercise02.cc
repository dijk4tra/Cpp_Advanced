#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace std;
using json = nlohmann::json;

struct Book {
    string title;
    string author;
    double price;
    string publisher;
    vector<string> tags;
};

const char* jsonstring = R"(
{
    "book": {
        "title": "JavaScript高级程序设计",
        "author": "Nicholas C. Zakas",
        "price": 129.00,
        "publisher": "人民邮电出版社",
        "tags": ["前端", "JavaScript", "编程"]
    }
})";

int main()
{
    // 解析JSON字符串
    json data = json::parse(jsonstring);
    // 创建并初始化Book对象
    Book book;
    book.title = data["book"]["title"];
    book.author = data["book"]["author"];
    book.price = data["book"]["price"];
    book.publisher = data["book"]["publisher"];
    book.tags = data["book"]["tags"];
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
}
