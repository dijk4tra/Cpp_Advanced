#include <iostream>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <tinyxml2.h>

using namespace std;
using json = nlohmann::json;
using namespace tinyxml2;

const char* jsonString = R"(
[
  {
    "@id": "B001",
    "author": "刘慈欣",
    "price": {
      "@currency": "CNY",
      "value": 68.0
    },
    "title": "三体",
    "year": 2008
  },
  {
    "@id": "B002",
    "author": "加西亚·马尔克斯",
    "price": {
      "@currency": "CNY",
      "value": 55.0
    },
    "title": "百年孤独",
    "year": 2011
  }
]
)";

// 将 double 类型价格转换成保留两位小数的字符串
string formatPrice(double price) {
    stringstream ss;
    ss << fixed << setprecision(2) << price;
    return ss.str();
}

int main() {
    // 1. 解析 JSON 字符串
    json books;

    try {
        books = json::parse(jsonString);
    } catch (const json::parse_error& e) {
        cerr << "JSON parse error: " << e.what() << endl;
        return -1;
    }

    // 2. 创建 XML 文档对象
    XMLDocument doc;

    // 3. 创建 XML 声明：<?xml version="1.0" encoding="UTF-8"?>
    XMLDeclaration* declaration = doc.NewDeclaration();
    doc.InsertEndChild(declaration);

    // 4. 创建根元素 <library>
    XMLElement* library = doc.NewElement("library");
    doc.InsertEndChild(library);

    // 5. 遍历 JSON 数组，每个对象转换成一个 <book> 元素
    for (const auto& item : books) {
        // 创建 <book> 元素
        XMLElement* book = doc.NewElement("book");

        // 设置 book 的 id 属性：<book id="B001">
        if (item.contains("@id")) {
            book->SetAttribute("id", item["@id"].get<string>().c_str());
        }

        // 创建 <title> 元素
        XMLElement* title = doc.NewElement("title");
        title->SetText(item["title"].get<string>().c_str());
        book->InsertEndChild(title);

        // 创建 <author> 元素
        XMLElement* author = doc.NewElement("author");
        author->SetText(item["author"].get<string>().c_str());
        book->InsertEndChild(author);

        // 创建 <year> 元素
        XMLElement* year = doc.NewElement("year");
        year->SetText(item["year"].get<int>());
        book->InsertEndChild(year);

        // 创建 <price> 元素
        XMLElement* price = doc.NewElement("price");

        // 设置 price 的 currency 属性：<price currency="CNY">
        if (item["price"].contains("@currency")) {
            price->SetAttribute(
                "currency",
                item["price"]["@currency"].get<string>().c_str()
            );
        }

        // 设置 price 的文本内容，例如：68.00
        double priceValue = item["price"]["value"].get<double>();
        string priceText = formatPrice(priceValue);
        price->SetText(priceText.c_str());

        // 将 <price> 添加到 <book>
        book->InsertEndChild(price);

        // 将 <book> 添加到 <library>
        library->InsertEndChild(book);
    }

    // 6. 保存 XML 到文件
    XMLError result = doc.SaveFile("library.xml");

    if (result != XML_SUCCESS) {
        cerr << "Failed to save XML file" << endl;
        return -1;
    }

    cout << "XML 文件保存成功：library.xml" << endl;

    return 0;
}
