#include "../../include/offline/PageProcessor.h"

#include "../../include/common/DirectoryScanner.h"
#include "../../include/common/TextUtils.h"

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
// 安全读取 parent 的指定子元素文本。
//
// tinyxml2 的 GetText() 返回指向 XMLDocument 内部存储的指针，这里立即复制成
// std::string，使调用者不依赖该指针的生命周期。缺少节点或文本时统一返回空串。
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

// 深度优先收集 XML 树中的全部 <item> 元素。
//
// 不同网页语料可能使用 rss/channel/item 等不同包装层级，因此不把路径写死。
// items 中保存的裸指针由 XMLDocument 持有；调用方必须在 XMLDocument 析构前
// 完成读取，本文件的 extract_documents 正是在当前文件循环内完成处理。
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

    // 无论当前节点是不是 item，都继续检查其子树，保证嵌套 item 不被遗漏。
    for (tinyxml2::XMLNode* child = node->FirstChild(); child; child = child->NextSibling()) {
        collect_items(child, items);
    }
}

// 按文本字节长度计算 SimHash 提取关键词数量。
//
// UTF-8 中文通常占多个字节，而 simhash 官方经验公式本身就是按 text.size()
// 的字节数估算，因此这里无需先换算为 Unicode 字符数。
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
    // Jieba 和 Simhasher 初始化时都需要加载词典，作为成员只构造一次。
    // 网页倒排索引使用中文停用词过滤高频低信息量词语。
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
    // 四个阶段具有严格的数据依赖：
    // 1. 从 XML 得到原始 documents_；
    // 2. 原地替换为去重文档并重新编号；
    // 3. 使用最终编号生成网页库和偏移库；
    // 4. 使用同一批文档生成倒排索引。
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
    // process 未来若被重复调用，先清空上一次结果，避免文档累积。
    documents_.clear();
    auto files = DirectoryScanner::scan(dir);

    // 此处 id 只是提取阶段的临时连续编号，SimHash 去重后还会再次编号。
    int nextId = 1;
    int rawItems = 0;

    for (const auto& file : files) {
        tinyxml2::XMLDocument xml;
        tinyxml2::XMLError err = xml.LoadFile(file.c_str());
        if (err != tinyxml2::XML_SUCCESS) {
            // 单个 XML 损坏时跳过该文件并继续处理其他语料；日志保留文件路径，
            // 便于离线任务结束后定位并修复原始数据。
            std::cerr << "[Page] skip invalid XML: " << file << std::endl;
            continue;
        }

        std::vector<tinyxml2::XMLElement*> items;
        collect_items(&xml, items);
        rawItems += static_cast<int>(items.size());

        for (tinyxml2::XMLElement* item : items) {
            // PDF 要求：优先使用 content；没有 content 时使用 description；
            // 两者都不存在或文本为空时，该 item 无法参与正文检索，直接忽略。
            std::string content = element_text(item, "content");
            if (content.empty()) {
                content = element_text(item, "description");
            }
            if (content.empty()) {
                continue;
            }

            // link 和 title 缺失不会阻止文档进入网页库，读取函数会返回空串。
            // content 已在上方保证非空。
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
    // uniqueDocuments 与 fingerprints 按相同下标一一对应，只为已经接受的
    // 非重复文档保存指纹。
    std::vector<Document> uniqueDocuments;
    std::vector<uint64_t> fingerprints;

    for (const auto& doc : documents_) {
        uint64_t hash = 0;
        // SimHash 将正文压缩成 64 位局部敏感指纹；相似文本倾向于拥有更小的
        // 汉明距离。topN 随正文长度调整，避免长短文档使用同一关键词数量。
        hasher_.make(doc.content, simhash_top_n(doc.content), hash);

        bool duplicated = false;
        // 将当前指纹与此前保留的每篇文档逐一比较。该课程实现复杂度为 O(n^2)，
        // 对给定离线语料规模足够直观；大规模数据可再使用分桶方法优化。
        for (uint64_t oldHash : fingerprints) {
            // 第三个参数明确指定阈值 3，即汉明距离 <= 3 视为重复。
            if (simhash::Simhasher::isEqual(hash, oldHash, 3)) {
                duplicated = true;
                break;
            }
        }

        if (!duplicated) {
            // 保留首次出现的文档，后续近似重复文档被丢弃。
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
    // 网页库用二进制模式打开，避免平台换行转换影响 tellp() 和实际写入字节数
    // 的一致性。偏移库本身是按行读取的纯文本。
    std::ofstream pageOfs(pages, std::ios::binary);
    if (!pageOfs) {
        throw std::runtime_error("failed to open pages file: " + pages);
    }

    std::ofstream offsetOfs(offsets);
    if (!offsetOfs) {
        throw std::runtime_error("failed to open offsets file: " + offsets);
    }

    for (const auto& doc : documents_) {
        // 先在内存中序列化一篇完整文档，之后才能准确得到该文档占用的字节数。
        // 对字段做 XML 转义，确保原正文中的标签字符不会破坏 pages.dat 结构。
        std::ostringstream oss;
        oss << "<doc>\n"
            << "  <id>" << doc.id << "</id>\n"
            << "  <link>" << TextUtils::escape_xml(doc.link) << "</link>\n"
            << "  <title>" << TextUtils::escape_xml(doc.title) << "</title>\n"
            << "  <content>" << TextUtils::escape_xml(doc.content) << "</content>\n"
            << "</doc>\n";

        std::string page = oss.str();
        // tellp() 在写入前返回当前输出位置，也就是该文档的起始字节偏移。
        std::streamoff offset = pageOfs.tellp();

        pageOfs.write(page.data(), static_cast<std::streamsize>(page.size()));
        // 偏移库格式：docId offset length。offset 和 length 都按字节计数，
        // 二期可据此 seek 到指定位置并一次读取完整 <doc>。
        offsetOfs << doc.id << ' ' << offset << ' ' << page.size() << '\n';
    }
}

void PageProcessor::build_inverted_index(const std::string& filename)
{
    // process 未来若重复调用，必须清除旧 posting list，避免混入上次结果。
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
        // 与中文词典建库一致，使用 Jieba 默认 Mix 模式切分网页正文。
        tokenizer_.Cut(doc.content, words);

        // DF 统计“包含该词的文档数”，同一词在一篇文档中无论出现多少次，
        // 都只能贡献 1，因此使用 set 记录本篇出现过的词。
        std::set<std::string> appearedInThisDoc;
        for (const auto& word : words) {
            // 停用词、空白、标点和纯数字不进入有效词总数，也不参与 TF-IDF。
            if (stopWords_.count(word) != 0) {
                continue;
            }
            if (TextUtils::is_useless_token(word)) {
                continue;
            }

            ++docTermCount[doc.id][word];
            // TF 的分母是过滤后的有效 token 总数，不是原始分词数组长度。
            ++docTotalWords[doc.id];
            appearedInThisDoc.insert(word);
        }

        // 遍历 set 而非原 words，保证同一文档对每个词的 DF 只累加一次。
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
            // PDF 公式：
            // TF  = 当前词在文档中的次数 / 当前文档有效词总数
            // IDF = log2(文档总数 / (包含该词的文档数 + 1))
            // w   = TF * IDF
            double tf = static_cast<double>(count) / docTotalWords[docId];
            double idf = std::log2(static_cast<double>(documentCount) / (documentFrequency[word] + 1));
            double weight = tf * idf;

            weights[word] = weight;
            // 同步累计平方和，后续计算文档权重向量的 L2 范数。
            squareSum += weight * weight;
        }

        double norm = std::sqrt(squareSum);
        // 当所有词的 IDF 都为 0 时，向量长度为 0，无法执行除法归一化。
        if (norm == 0.0) {
            continue;
        }

        for (const auto& [word, weight] : weights) {
            // 把正向结构 docId -> word -> weight 反转为
            // word -> docId -> normalizedWeight，便于二期按查询词快速找文档。
            invertedIndex_[word][docId] = weight / norm;
        }
    }

    // 输出格式：word docId weight [docId weight]...
    // map 保证关键词和 docId 顺序稳定；固定 10 位小数减少序列化精度损失。
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
