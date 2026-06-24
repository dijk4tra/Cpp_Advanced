#include "../../include/online/PageLibrary.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tinyxml2.h>
#include <utility>

namespace
{
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

void PageLibrary::load(const std::string& pagesFile, const std::string& offsetsFile)
{
    load_offsets(offsetsFile);
    documents_.clear();

    std::ifstream ifs(pagesFile, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("failed to open pages file: " + pagesFile);
    }

    for (const auto& [docId, offset] : offsets_) {
        std::string xmlText(offset.length, '\0');
        ifs.seekg(static_cast<std::streamoff>(offset.offset));
        ifs.read(xmlText.data(), static_cast<std::streamsize>(offset.length));
        if (!ifs) {
            throw std::runtime_error("failed to read page fragment from: " + pagesFile);
        }

        Document doc = parse_document(xmlText);
        if (doc.id != 0) {
            documents_[doc.id] = std::move(doc);
        }
    }
}

const Document* PageLibrary::find(int docId) const
{
    auto it = documents_.find(docId);
    if (it == documents_.end()) {
        return nullptr;
    }
    return &it->second;
}

void PageLibrary::load_offsets(const std::string& offsetsFile)
{
    std::ifstream ifs(offsetsFile);
    if (!ifs) {
        throw std::runtime_error("failed to open offsets file: " + offsetsFile);
    }

    offsets_.clear();
    PageOffset offset;
    while (ifs >> offset.docId >> offset.offset >> offset.length) {
        offsets_[offset.docId] = offset;
    }
}

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
    std::istringstream idStream(element_text(root, "id"));
    idStream >> doc.id;
    doc.link = element_text(root, "link");
    doc.title = element_text(root, "title");
    doc.content = element_text(root, "content");
    return doc;
}
