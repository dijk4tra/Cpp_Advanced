#include "../../include/online/PageLibrary.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tinyxml2.h>

namespace
{
// 该辅助函数只用于解析 pages.dat 中的单篇 <doc>，不需要出现在头文件中。

/**
 * @brief 读取 XML 父元素中指定子元素的文本。
 *
 * tinyxml2 返回的文本指针由 XMLDocument 管理，本函数立即复制成 std::string，
 * 避免调用者依赖 XMLDocument 内部内存。
 *
 * @param parent 父元素，可以为 nullptr。
 * @param childName 子元素标签名。
 * @return 子元素文本；不存在时返回空字符串。
 */
std::string element_text(tinyxml2::XMLElement* parent, const char* childName)
{
    if (parent == nullptr) {
        return "";
    }

    // FirstChildElement 只查找直接子元素。这里 pages.dat 的结构固定为
    // <doc><id>...</id><link>...</link>...</doc>，不需要递归查找。
    tinyxml2::XMLElement* child = parent->FirstChildElement(childName);
    if (child == nullptr || child->GetText() == nullptr) {
        return "";
    }
    // GetText() 返回 const char*，直接 return 会构造 std::string 副本。
    return child->GetText();
}
}

/**
 * @brief 加载偏移库并记录网页库文件路径。
 * @throws std::runtime_error offsets 文件无法打开，或 pages 文件不可读时抛出。
 */
void PageLibrary::load(const std::string& pagesFile, const std::string& offsetsFile)
{
    pagesFile_ = pagesFile;

    // 先验证 pages.dat 可读。真正读取文档发生在 find()，这里不全量解析网页库。
    std::ifstream pageCheck(pagesFile_, std::ios::binary);
    if (!pageCheck) {
        throw std::runtime_error("failed to open pages file: " + pagesFile_);
    }

    // 加载偏移库，得到每篇文档在 pages.dat 中的字节范围。
    load_offsets(offsetsFile);
}

/**
 * @brief 根据文档 id 按需读取文档。
 */
bool PageLibrary::find(int docId, Document& doc) const
{
    auto it = offsets_.find(docId);
    if (it == offsets_.end()) {
        return false;
    }

    std::ifstream ifs(pagesFile_, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("failed to open pages file: " + pagesFile_);
    }

    const PageOffset& offset = it->second;
    std::string xmlText(offset.length, '\0');
    ifs.seekg(static_cast<std::streamoff>(offset.offset));
    ifs.read(xmlText.data(), static_cast<std::streamsize>(offset.length));
    if (!ifs) {
        throw std::runtime_error("failed to read page fragment from: " + pagesFile_);
    }

    doc = parse_document(xmlText);
    return doc.id != 0;
}

/**
 * @brief 加载偏移库。
 * @throws std::runtime_error 文件无法打开时抛出。
 */
void PageLibrary::load_offsets(const std::string& offsetsFile)
{
    std::ifstream ifs(offsetsFile);
    if (!ifs) {
        throw std::runtime_error("failed to open offsets file: " + offsetsFile);
    }

    offsets_.clear();
    PageOffset offset;
    // offsets.dat 每行三列：docId 起始偏移 字节长度。
    // operator>> 会自动跳过空白，读到 EOF 时循环结束。
    while (ifs >> offset.docId >> offset.offset >> offset.length) {
        // 如果出现重复 docId，后读到的记录会覆盖前面的记录。
        offsets_[offset.docId] = offset;
    }
}

/**
 * @brief 解析单篇 `<doc>` XML 片段。
 *
 * 解析失败时返回空 Document，由 load() 跳过。这样单篇损坏不会使整个进程崩溃，
 * 但正常建库产生的 pages.dat 不应出现解析失败。
 */
Document PageLibrary::parse_document(const std::string& xmlText) const
{
    tinyxml2::XMLDocument xml;
    // Parse 从内存字符串中解析 XML。第二个参数传入字节长度，避免依赖字符串以
    // '\0' 结尾之外的额外假设。
    if (xml.Parse(xmlText.c_str(), xmlText.size()) != tinyxml2::XML_SUCCESS) {
        return Document{};
    }

    // pages.dat 中每篇文档最外层标签应为 <doc>。
    tinyxml2::XMLElement* root = xml.FirstChildElement("doc");
    if (root == nullptr) {
        return Document{};
    }

    Document doc;
    // id 以文本形式保存在 XML 中，通过字符串流转为 int。
    std::istringstream idStream(element_text(root, "id"));
    idStream >> doc.id;
    // link/title/content 缺失时 element_text 返回空字符串，不影响程序继续运行。
    doc.link = element_text(root, "link");
    doc.title = element_text(root, "title");
    doc.content = element_text(root, "content");
    return doc;
}
