#include "../../include/online/PageLibrary.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tinyxml2.h>
#include <utility>

namespace
{
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

    tinyxml2::XMLElement* child = parent->FirstChildElement(childName);
    if (child == nullptr || child->GetText() == nullptr) {
        return "";
    }
    return child->GetText();
}
}

/**
 * @brief 将网页库和偏移库加载到内存。
 * @throws std::runtime_error 文件无法打开或读取片段失败时抛出。
 */
void PageLibrary::load(const std::string& pagesFile, const std::string& offsetsFile)
{
    // 先加载偏移库，得到每篇文档在 pages.dat 中的字节范围。
    load_offsets(offsetsFile);
    documents_.clear();

    // pages.dat 按字节偏移读取，必须用二进制模式打开，避免换行转换影响 seekg。
    std::ifstream ifs(pagesFile, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("failed to open pages file: " + pagesFile);
    }

    for (const auto& [docId, offset] : offsets_) {
        // offset.length 是该 doc XML 片段的字节长度。先创建等长字符串作为读取缓冲区。
        std::string xmlText(offset.length, '\0');
        ifs.seekg(static_cast<std::streamoff>(offset.offset));
        ifs.read(xmlText.data(), static_cast<std::streamsize>(offset.length));
        if (!ifs) {
            throw std::runtime_error("failed to read page fragment from: " + pagesFile);
        }

        Document doc = parse_document(xmlText);
        if (doc.id != 0) {
            // 用 XML 内部的 id 作为 key，与倒排索引中的 docId 保持一致。
            documents_[doc.id] = std::move(doc);
        }
    }
}

/**
 * @brief 根据文档 id 查找已加载文档。
 */
const Document* PageLibrary::find(int docId) const
{
    auto it = documents_.find(docId);
    if (it == documents_.end()) {
        return nullptr;
    }
    return &it->second;
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
    while (ifs >> offset.docId >> offset.offset >> offset.length) {
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
    if (xml.Parse(xmlText.c_str(), xmlText.size()) != tinyxml2::XML_SUCCESS) {
        return Document{};
    }

    tinyxml2::XMLElement* root = xml.FirstChildElement("doc");
    if (root == nullptr) {
        return Document{};
    }

    Document doc;
    // id 以文本形式保存在 XML 中，通过字符串流转为 int。
    std::istringstream idStream(element_text(root, "id"));
    idStream >> doc.id;
    doc.link = element_text(root, "link");
    doc.title = element_text(root, "title");
    doc.content = element_text(root, "content");
    return doc;
}
