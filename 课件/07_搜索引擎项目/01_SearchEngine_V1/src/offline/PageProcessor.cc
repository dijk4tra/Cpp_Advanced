#include "offline/PageProcessor.h"

#include "common/DirectoryScanner.h"
#include "common/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tinyxml2.h>

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

void collect_items(tinyxml2::XMLNode* node, std::vector<tinyxml2::XMLElement*>& items)
{
    if (node == nullptr) {
        return;
    }

    if (auto* element = node->ToElement()) {
        if (std::string(element->Name()) == "item") {
            items.push_back(element);
        }
    }

    // XML 文件的层级可能是 rss/channel/item，也可能是其他结构。
    // 这里递归遍历，保证只要文件中存在 item 标签就能提取出来。
    for (tinyxml2::XMLNode* child = node->FirstChild(); child; child = child->NextSibling()) {
        collect_items(child, items);
    }
}

int simhash_top_n(const std::string& text)
{
    // PDF 中给出的推荐规则：max(5, min(200, text.size() / 120))。
    int topN = static_cast<int>(text.size() / 120);
    topN = std::max(5, topN);
    topN = std::min(200, topN);
    return topN;
}
}

PageProcessor::PageProcessor(const std::string& stopWordsFile)
    : tokenizer_()
    , hasher_()
    , stopWords_(TextUtils::load_stop_words(stopWordsFile))
{
}

void PageProcessor::process(const std::string& dir,
                            const std::string& pages,
                            const std::string& offsets,
                            const std::string& invertIndex)
{
    std::cout << "[Page] extract documents..." << std::endl;
    extract_documents(dir);

    std::cout << "[Page] deduplicate documents..." << std::endl;
    deduplicate_documents();

    std::cout << "[Page] build pages and offsets..." << std::endl;
    build_pages_and_offsets(pages, offsets);

    std::cout << "[Page] build inverted index..." << std::endl;
    build_inverted_index(invertIndex);
}

void PageProcessor::extract_documents(const std::string& dir)
{
    documents_.clear();
    auto files = DirectoryScanner::scan(dir);

    int nextId = 1;
    int rawItems = 0;

    for (const auto& file : files) {
        tinyxml2::XMLDocument xml;
        tinyxml2::XMLError err = xml.LoadFile(file.c_str());
        if (err != tinyxml2::XML_SUCCESS) {
            std::cerr << "[Page] skip invalid XML: " << file << std::endl;
            continue;
        }

        std::vector<tinyxml2::XMLElement*> items;
        collect_items(&xml, items);
        rawItems += static_cast<int>(items.size());

        for (tinyxml2::XMLElement* item : items) {
            // PDF 要求：优先使用 content；没有 content 时使用 description；都没有就忽略。
            std::string content = element_text(item, "content");
            if (content.empty()) {
                content = element_text(item, "description");
            }
            if (content.empty()) {
                continue;
            }

            Document doc;
            doc.id = nextId++;
            doc.link = element_text(item, "link");
            doc.title = element_text(item, "title");
            doc.content = content;
            documents_.push_back(std::move(doc));
        }
    }

    std::cout << "[Page] XML files: " << files.size()
              << ", raw items: " << rawItems
              << ", valid documents: " << documents_.size() << std::endl;
}

void PageProcessor::deduplicate_documents()
{
    std::vector<Document> uniqueDocuments;
    std::vector<uint64_t> fingerprints;

    for (const auto& doc : documents_) {
        uint64_t hash = 0;
        hasher_.make(doc.content, simhash_top_n(doc.content), hash);

        bool duplicated = false;
        for (uint64_t oldHash : fingerprints) {
            // PDF 要求：汉明距离在 3 以内认为文档十分相似。
            if (simhash::Simhasher::isEqual(hash, oldHash, 3)) {
                duplicated = true;
                break;
            }
        }

        if (!duplicated) {
            uniqueDocuments.push_back(doc);
            fingerprints.push_back(hash);
        }
    }

    documents_.swap(uniqueDocuments);

    // 去重后重新编号，保证 id 连续，并且与 pages/offsets/invert_index 一致。
    for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
        documents_[i].id = i + 1;
    }

    std::cout << "[Page] unique documents: " << documents_.size() << std::endl;
}

void PageProcessor::build_pages_and_offsets(const std::string& pages,
                                            const std::string& offsets)
{
    std::ofstream pageOfs(pages, std::ios::binary);
    if (!pageOfs) {
        throw std::runtime_error("failed to open pages file: " + pages);
    }

    std::ofstream offsetOfs(offsets);
    if (!offsetOfs) {
        throw std::runtime_error("failed to open offsets file: " + offsets);
    }

    for (const auto& doc : documents_) {
        std::ostringstream oss;
        oss << "<doc>\n"
            << "  <id>" << doc.id << "</id>\n"
            << "  <link>" << TextUtils::escape_xml(doc.link) << "</link>\n"
            << "  <title>" << TextUtils::escape_xml(doc.title) << "</title>\n"
            << "  <content>" << TextUtils::escape_xml(doc.content) << "</content>\n"
            << "</doc>\n";

        std::string page = oss.str();
        std::streamoff offset = pageOfs.tellp();

        pageOfs.write(page.data(), static_cast<std::streamsize>(page.size()));
        offsetOfs << doc.id << ' ' << offset << ' ' << page.size() << '\n';
    }
}

void PageProcessor::build_inverted_index(const std::string& filename)
{
    invertedIndex_.clear();

    // 第一步：统计每篇文档中每个词出现的次数。
    // docTermCount[docId][word] = count
    std::map<int, std::map<std::string, int>> docTermCount;

    // docTotalWords[docId] = 当前文档有效词总数，用于计算 TF。
    std::map<int, int> docTotalWords;

    // documentFrequency[word] = 有多少篇文档包含该词，用于计算 IDF。
    std::map<std::string, int> documentFrequency;

    for (const auto& doc : documents_) {
        std::vector<std::string> words;
        tokenizer_.Cut(doc.content, words);

        std::set<std::string> appearedInThisDoc;
        for (const auto& word : words) {
            if (stopWords_.count(word) != 0) {
                continue;
            }
            if (TextUtils::is_useless_token(word)) {
                continue;
            }

            ++docTermCount[doc.id][word];
            ++docTotalWords[doc.id];
            appearedInThisDoc.insert(word);
        }

        for (const auto& word : appearedInThisDoc) {
            ++documentFrequency[word];
        }
    }

    const int documentCount = static_cast<int>(documents_.size());

    // 第二步：对每篇文档计算 TF-IDF，并按文档向量长度归一化。
    for (const auto& [docId, terms] : docTermCount) {
        if (docTotalWords[docId] == 0) {
            continue;
        }

        std::map<std::string, double> weights;
        double squareSum = 0.0;

        for (const auto& [word, count] : terms) {
            double tf = static_cast<double>(count) / docTotalWords[docId];
            double idf = std::log2(static_cast<double>(documentCount) / (documentFrequency[word] + 1));
            double weight = tf * idf;

            weights[word] = weight;
            squareSum += weight * weight;
        }

        double norm = std::sqrt(squareSum);
        if (norm == 0.0) {
            continue;
        }

        for (const auto& [word, weight] : weights) {
            invertedIndex_[word][docId] = weight / norm;
        }
    }

    std::ofstream ofs(filename);
    if (!ofs) {
        throw std::runtime_error("failed to open inverted index file: " + filename);
    }

    ofs << std::fixed << std::setprecision(10);
    for (const auto& [word, postingList] : invertedIndex_) {
        ofs << word;
        for (const auto& [docId, weight] : postingList) {
            ofs << ' ' << docId << ' ' << weight;
        }
        ofs << '\n';
    }

    std::cout << "[Page] inverted index keywords: " << invertedIndex_.size() << std::endl;
}
