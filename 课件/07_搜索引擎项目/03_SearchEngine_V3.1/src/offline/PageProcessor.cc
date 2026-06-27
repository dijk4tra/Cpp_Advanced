#include "../../include/offline/PageProcessor.h"

#include "../../include/common/DirectoryScanner.h"
#include "../../include/common/Logger.h"
#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <tinyxml2.h>

namespace
{
// 匿名命名空间中的 XML 和 SimHash 辅助函数不会暴露到其他翻译单元。

/**
 * @brief 安全读取 XML 父元素中指定子元素的文本。
 *
 * tinyxml2 的 GetText() 返回指向 XMLDocument 内部存储的指针，本函数立即复制
 * 为 std::string，使返回值不依赖该指针的生命周期。
 *
 * @param parent 要查询的父元素，可以为 nullptr。
 * @param childName 子元素标签名，必须是有效的 C 字符串。
 * @return 子元素文本副本；父元素、子元素或文本不存在时返回空字符串。
 * @throws std::bad_alloc 构造返回字符串时内存分配失败可能抛出。
 */
std::string element_text(tinyxml2::XMLElement* parent, const char* childName)
{
    if (parent == nullptr) {
        return "";
    }

    // `->` 用于通过指针访问对象成员；前面的 nullptr 检查保证解引用安全。
    tinyxml2::XMLElement* child = parent->FirstChildElement(childName);
    if (child == nullptr || child->GetText() == nullptr) {
        return "";
    }

    return child->GetText();
}

/**
 * @brief 递归收集 XML 子树中的全部 `<item>` 元素。
 *
 * 不同语料可能使用 rss/channel/item 等不同包装层级，因此采用深度优先遍历，
 * 不把元素路径写死。items 中的裸指针由 XMLDocument 持有，调用者必须在
 * XMLDocument 析构前使用完毕。
 *
 * @param node 当前遍历节点，可以为 nullptr。
 * @param items 输出参数，找到的 item 元素指针会追加到该 vector。
 * @throws std::bad_alloc items 扩容时内存分配失败可能抛出。
 */
void collect_items(tinyxml2::XMLNode* node, std::vector<tinyxml2::XMLElement*>& items)
{
    if (node == nullptr) {
        return;
    }

    // C++17 允许在 if 条件中声明并初始化变量。ToElement() 对非元素节点返回
    // nullptr，指针转换为 bool 后决定是否进入分支。
    if (auto* element = node->ToElement()) {
        if (std::string(element->Name()) == "item") {
            items.push_back(element);
        }
    }

    // 无论当前节点是不是 item，都继续检查其子树，保证嵌套 item 不被遗漏。
    // for 的三个部分分别负责取得第一个子节点、检查指针非空、移动到下一个
    // 兄弟节点；对每个子节点递归即形成深度优先遍历。
    for (tinyxml2::XMLNode* child = node->FirstChild(); child; child = child->NextSibling()) {
        collect_items(child, items);
    }
}

/**
 * @brief 按正文 UTF-8 字节长度计算 SimHash 的 topN 参数。
 *
 * 采用 PDF 推荐公式 `max(5, min(200, text.size()/120))`，将结果限制在
 * `[5, 200]`。公式按字节数估算，无需换算 Unicode 字符数。
 *
 * @param text 网页正文。
 * @return 传给 Simhasher::make 的关键词数量。
 * @note 异常处理：本函数不抛出异常。
 */
int simhash_top_n(const std::string& text)
{
    // PDF 中给出的推荐规则：max(5, min(200, text.size() / 120))。
    // size() 返回无符号 size_t；除法后结果最多受上限 200 约束，此处显式转换
    // 为 Simhasher 接口需要的 int。
    int topN = static_cast<int>(text.size() / 120);
    topN = std::max(5, topN);
    topN = std::min(200, topN);
    return topN;
}
}

/**
 * @brief 构造网页搜索离线处理器。
 * @param stopWordsFile 中文停用词文件路径。
 * @throws std::runtime_error 停用词文件无法打开时抛出。
 * @throws cppjieba 或 simhash 初始化失败时产生的异常会自然传播。
 */
PageProcessor::PageProcessor(const std::string& stopWordsFile)
    // Jieba 和 Simhasher 初始化时都需要加载词典，作为成员只构造一次。
    // 网页倒排索引使用中文停用词过滤高频低信息量词语。
    : tokenizer_()
    , hasher_()
    , stopWords_(TextUtils::load_stop_words(stopWordsFile))
{
}

/**
 * @brief 按固定依赖顺序生成网页库、偏移库、倒排索引和 BM25 文档统计。
 * @param dir 网页 XML 语料目录。
 * @param pages 网页库输出路径。
 * @param offsets 偏移库输出路径。
 * @param invertIndex 倒排索引输出路径。
 * @param docStats BM25 文档统计输出路径。
 * @throws std::runtime_error 目录无法扫描或任一输出文件无法打开时抛出。
 * @throws utf8::exception 分词结果编码非法时可能抛出。
 */
void PageProcessor::process(const std::string& dir,
                            const std::string& pages,
                            const std::string& offsets,
                            const std::string& invertIndex,
                            const std::string& docStats)
{
    // 四个阶段具有严格的数据依赖：
    // 1. 从 XML 得到原始 documents_；
    // 2. 原地替换为去重文档并重新编号；
    // 3. 使用最终编号生成网页库和偏移库；
    // 4. 使用同一批文档生成倒排索引。
    // steady_clock 不受系统时钟回拨影响，用它记录离线各阶段的真实耗时。
    // 同一个 time_point 变量在阶段切换时重置，使日志可直接定位慢阶段。
    auto started = std::chrono::steady_clock::now();
    spdlog::info("offline stage started stage=extract_documents");
    extract_documents(dir);
    spdlog::info("offline stage finished stage=extract_documents elapsed_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started).count());

    // duration_cast<milliseconds> 显式统一日志单位，count() 返回整数毫秒数。
    started = std::chrono::steady_clock::now();
    spdlog::info("offline stage started stage=deduplicate_documents");
    deduplicate_documents();
    spdlog::info("offline stage finished stage=deduplicate_documents elapsed_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started).count());

    started = std::chrono::steady_clock::now();
    spdlog::info("offline stage started stage=pages_and_offsets");
    build_pages_and_offsets(pages, offsets);
    spdlog::info("offline stage finished stage=pages_and_offsets elapsed_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started).count());

    started = std::chrono::steady_clock::now();
    spdlog::info("offline stage started stage=bm25_index");
    build_inverted_index(invertIndex, docStats);
    spdlog::info("offline stage finished stage=bm25_index elapsed_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started).count());
}

/**
 * @brief 从目录内所有可解析 XML 文件提取有效文档。
 * @param dir 网页 XML 语料目录。
 * @throws std::runtime_error dir 无法扫描时抛出。
 * @note 单个 XML 文件加载失败只输出错误日志并跳过，不中断其他文件处理。
 */
void PageProcessor::extract_documents(const std::string& dir)
{
    // 阶段 1：重置状态并取得稳定排序后的 XML 文件列表。
    // process 未来若被重复调用，先清空上一次结果，避免文档累积。
    documents_.clear();
    auto files = DirectoryScanner::scan(dir);

    // 此处 id 只是提取阶段的临时连续编号，SimHash 去重后还会再次编号。
    int nextId = 1;
    int rawItems = 0;

    // 阶段 2：逐文件解析。XMLDocument 是当前循环迭代中的栈对象，离开本轮
    // 循环即析构，因此所有元素指针必须在本轮转换为自有 std::string。
    for (const auto& file : files) {
        tinyxml2::XMLDocument xml;
        // tinyxml2 以错误码报告解析状态，而不是为普通解析错误抛异常。
        tinyxml2::XMLError err = xml.LoadFile(file.c_str());
        if (err != tinyxml2::XML_SUCCESS) {
            // 单个 XML 损坏时跳过该文件并继续处理其他语料；日志保留文件路径，
            // 便于离线任务结束后定位并修复原始数据。
            spdlog::warn("invalid XML skipped file={} tinyxml_error={}", file,
                         static_cast<int>(err));
            continue;
        }

        // vector 保存非拥有型指针：XMLDocument 负责节点内存，items 只暂时引用。
        std::vector<tinyxml2::XMLElement*> items;
        collect_items(&xml, items);
        rawItems += static_cast<int>(items.size());

        // 阶段 3：按 PDF 的 content/description 优先级生成 Document。
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
            // std::move 将 doc 转为右值，使 vector 可移动其 string 成员，避免复制
            // 正文等大字符串。移动后 doc 不再使用，并在本轮末尾正常析构。
            documents_.push_back(std::move(doc));
        }
    }

    spdlog::info("documents extracted xml_files={} raw_items={} valid_documents={}",
                 files.size(), rawItems, documents_.size());
}

/**
 * @brief 使用 SimHash 删除近似重复文档并重新连续编号。
 * @throws simhash 或其分词依赖产生的异常会自然传播。
 *
 * @details 当前实现保留每组近似文档中首次出现的一篇。每篇新文档都与此前
 * 保留文档比较，因此时间复杂度为 O(n^2)，额外空间复杂度为 O(n)。
 */
void PageProcessor::deduplicate_documents()
{
    // 阶段 1：计算指纹，并选择首次出现的非重复文档。
    // uniqueDocuments 与 fingerprints 按相同下标一一对应，只为已经接受的
    // 非重复文档保存指纹。
    std::vector<Document> uniqueDocuments;
    std::vector<uint64_t> fingerprints;

    for (const auto& doc : documents_) {
        uint64_t hash = 0;
        // SimHash 将正文压缩成 64 位局部敏感指纹；相似文本倾向于拥有更小的
        // 汉明距离。topN 随正文长度调整，避免长短文档使用同一关键词数量。
        // hash 通过引用参数写回；函数返回后保存正文对应的 64 位指纹。
        hasher_.make(doc.content, simhash_top_n(doc.content), hash);

        bool duplicated = false;
        // 将当前指纹与此前保留的每篇文档逐一比较。该课程实现复杂度为 O(n^2)，
        // 对给定离线语料规模足够直观；大规模数据可再使用分桶方法优化。
        // uint64_t 固定为 64 位无符号整数，正好容纳完整 SimHash 指纹。
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

    // 阶段 2：swap 以常数复杂度交换两个 vector 的内部缓冲区。documents_
    // 获得去重结果，旧原始文档随 uniqueDocuments 离开作用域自动释放。
    documents_.swap(uniqueDocuments);

    // 去重后重新编号，保证 id 连续，并且与 pages/offsets/invert_index 一致。
    // vector 下标从 0 开始，而外部文档 id 按课程格式从 1 开始，所以赋值 i+1。
    // static_cast 明确把 size_t 转成 int，与 Document::id 类型保持一致。
    for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
        documents_[i].id = i + 1;
    }

    spdlog::info("documents deduplicated unique_documents={}", documents_.size());
}

/**
 * @brief 写出网页库以及按字节定位每篇网页的偏移库。
 * @param pages 网页库输出路径。
 * @param offsets 偏移库输出路径。
 * @throws std::runtime_error 任一输出文件无法打开时抛出。
 */
void PageProcessor::build_pages_and_offsets(const std::string& pages,
                                            const std::string& offsets)
{
    // 阶段 1：以截断模式创建两个输出流。流对象离开函数时会自动 flush 和 close。
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
        // `<<` 连续返回同一个输出流引用，因此可以链式拼接多个字段。
        std::ostringstream oss;
        oss << "<doc>\n"
            << "  <id>" << doc.id << "</id>\n"
            << "  <link>" << TextUtils::escape_xml(doc.link) << "</link>\n"
            << "  <title>" << TextUtils::escape_xml(doc.title) << "</title>\n"
            << "  <content>" << TextUtils::escape_xml(doc.content) << "</content>\n"
            << "</doc>\n";

        std::string page = oss.str();
        // tellp() 在写入前返回当前输出位置，也就是该文档的起始字节偏移。
        // streamoff 是专门表示流位置差值的有符号类型，比 int 更适合大文件。
        std::streamoff offset = pageOfs.tellp();

        // write 执行非格式化二进制写入，不会把 '\n' 或正文内容再次转换。
        // size_t 显式转为 streamsize，以匹配 write 的第二个参数类型。
        pageOfs.write(page.data(), static_cast<std::streamsize>(page.size()));
        // 偏移库格式：docId offset length。offset 和 length 都按字节计数，
        // 二期可据此 seek 到指定位置并一次读取完整 <doc>。
        offsetOfs << doc.id << ' ' << offset << ' ' << page.size() << '\n';
    }
}

/**
 * @brief 统计 BM25 所需信息，并写出倒排索引和文档长度统计。
 * @param indexFile 倒排索引输出路径。
 * @param statsFile BM25 文档统计输出路径。
 * @throws std::runtime_error 输出文件无法打开时抛出。
 * @throws utf8::exception 分词结果编码非法时可能抛出。
 */
void PageProcessor::build_inverted_index(const std::string& indexFile,
                                         const std::string& statsFile)
{
    // 阶段 1：清除旧索引并准备 BM25 需要的原始词频和文档长度。
    // process 未来若重复调用，必须清除旧 posting list，避免混入上次结果。
    invertedIndex_.clear();

    // 第一步：统计每篇文档中每个词出现的次数。
    // docTermCount[docId][word] = count
    // 外层 key 是 docId，内层 key 是 word，内层 value 是当前文档中的出现次数。
    std::map<int, std::map<std::string, int>> docTermCount;

    // docTotalWords[docId] = 当前文档有效 token 数，即 BM25 的 dl。
    std::map<int, int> docTotalWords;

    // 阶段 2：逐文档分词，同时统计原始 tf 和 dl。
    for (const auto& doc : documents_) {
        // 即使文档过滤后没有有效 token，也要在统计库中保存 dl=0。
        docTotalWords[doc.id] = 0;
        std::vector<std::string> words;
        // 与中文词典建库一致，使用 Jieba 默认 Mix 模式切分网页正文。
        tokenizer_.Cut(doc.content, words);

        for (const auto& word : words) {
            // 停用词、空白、标点和纯数字不进入 BM25 的 tf/dl 统计。
            if (stopWords_.count(word) != 0) {
                continue;
            }
            if (TextUtils::is_useless_token(word)) {
                continue;
            }

            // 连续两个 [] 分别访问外层 docId 和内层 word；不存在的映射会被
            // 默认构造，int 初始为 0，随后通过 ++ 累计。
            ++docTermCount[doc.id][word];
            // dl 是过滤后的有效 token 总数，不是原始分词数组长度。
            ++docTotalWords[doc.id];
        }
    }

    // 阶段 3：把正向 tf 统计反转为 word -> docId -> tf。posting list 的长度
    // 就是 df，无需重复维护另一份 documentFrequency 映射。
    const int documentCount = static_cast<int>(documents_.size());
    for (const auto& [docId, terms] : docTermCount) {
        for (const auto& [word, count] : terms) {
            invertedIndex_[word][docId] = count;
        }
    }

    // 阶段 4：输出 word df docId tf [docId tf]...。
    std::ofstream indexOfs(indexFile);
    if (!indexOfs) {
        throw std::runtime_error("failed to open inverted index file: " + indexFile);
    }
    for (const auto& [word, postingList] : invertedIndex_) {
        indexOfs << word << ' ' << postingList.size();
        for (const auto& [docId, tf] : postingList) {
            indexOfs << ' ' << docId << ' ' << tf;
        }
        indexOfs << '\n';
    }

    // 阶段 5：输出版本化 BM25 文档统计。avgdl 按全部去重文档计算，包含 dl=0
    // 的文档，使 N 与网页库/偏移库的文档总数保持一致。
    std::uint64_t totalDocumentLength = 0;
    for (const auto& [docId, dl] : docTotalWords) {
        totalDocumentLength += static_cast<std::uint64_t>(dl);
    }
    const double averageDocumentLength = documentCount == 0
        ? 0.0
        : static_cast<double>(totalDocumentLength) / documentCount;

    std::ofstream statsOfs(statsFile);
    if (!statsOfs) {
        throw std::runtime_error("failed to open BM25 document stats file: " + statsFile);
    }
    statsOfs << "BM25_STATS_V1 " << documentCount << ' '
             << std::fixed << std::setprecision(10) << averageDocumentLength << '\n';
    for (const auto& [docId, dl] : docTotalWords) {
        statsOfs << docId << ' ' << dl << '\n';
    }

    spdlog::info("BM25 index built terms={} documents={} avgdl={:.3f}",
                 invertedIndex_.size(), documentCount, averageDocumentLength);
}
