#include "../../include/offline/KeywordProcessor.h"

#include "../../include/common/DirectoryScanner.h"
#include "../../include/common/TextUtils.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

/**
 * @brief 构造关键字推荐离线处理器。
 * @param enStopWordsFile 英文停用词文件路径。
 * @param cnStopWordsFile 中文停用词文件路径。
 * @throws std::runtime_error 任一停用词文件无法打开时抛出。
 * @throws cppjieba 初始化失败时产生的异常会自然传播。
 */
KeywordProcessor::KeywordProcessor(const std::string& enStopWordsFile,
                                   const std::string& cnStopWordsFile)
    // Jieba 构造时会加载分词词典，开销较大，因此作为成员只初始化一次。
    // 停用词也在构造阶段一次性读入内存，后续扫描大量 token 时直接查集合。
    : tokenizer_()
    , enStopWords_(TextUtils::load_stop_words(enStopWordsFile))
    , cnStopWords_(TextUtils::load_stop_words(cnStopWordsFile))
{
}

/**
 * @brief 依次构建中英文词典和字符索引。
 * @param cnDir 中文语料目录。
 * @param enDir 英文语料目录。
 * @param cnDict 中文词典输出路径。
 * @param cnIndex 中文字符索引输出路径。
 * @param enDict 英文词典输出路径。
 * @param enIndex 英文字符索引输出路径。
 * @throws std::runtime_error 任一输入或输出路径不可用时抛出。
 * @throws utf8::exception 中文文本编码非法时可能抛出。
 */
void KeywordProcessor::process(const std::string& cnDir,
                               const std::string& enDir,
                               const std::string& cnDict,
                               const std::string& cnIndex,
                               const std::string& enDict,
                               const std::string& enIndex)
{
    // 索引中保存的是词典文件的行号，因此每种语言都必须先生成词典，
    // 再读取该词典构建对应索引，不能颠倒顺序。
    std::cout << "[Keyword] build English dictionary..." << std::endl;
    create_en_dict(enDir, enDict);

    std::cout << "[Keyword] build English index..." << std::endl;
    build_en_index(enDict, enIndex);

    std::cout << "[Keyword] build Chinese dictionary..." << std::endl;
    create_cn_dict(cnDir, cnDict);

    std::cout << "[Keyword] build Chinese index..." << std::endl;
    build_cn_index(cnDict, cnIndex);
}

/**
 * @brief 清洗英文语料并生成按单词排序的词频词典。
 * @param dir 英文语料目录。
 * @param outfile 英文词典输出路径。
 * @throws std::runtime_error 目录无法扫描、语料无法读取或输出无法打开时抛出。
 */
void KeywordProcessor::create_en_dict(const std::string& dir, const std::string& outfile)
{
    // 阶段 1：扫描目录，准备全局词频表。
    // map 同时承担词频统计和字典序排序。按 key 有序输出后，相同语料每次生成
    // 的词典行号都保持稳定，这对行号型索引非常重要。
    std::map<std::string, int> wordFrequency;
    auto files = DirectoryScanner::scan(dir);

    // 范围 for 逐个访问 vector 中的路径；const 引用避免复制较长的字符串。
    for (const auto& file : files) {
        // 任何语料文件无法读取都视为建库失败，避免只处理部分语料却生成
        // 看似完整的词典。
        std::ifstream ifs(file);
        if (!ifs) {
            throw std::runtime_error("failed to open English corpus file: " + file);
        }

        std::string line;
        // 阶段 2：逐行清洗和分词，避免一次将整个英文语料文件读入内存。
        while (std::getline(ifs, line)) {
            // 英文语料要求：数字和标点替换为空格，只保留字母，并统一小写。
            std::string normalized = TextUtils::normalize_english_line(line);

            std::istringstream iss(normalized);
            std::string word;
            // 归一化已经把非字母替换为空格，因此流提取即可得到纯小写单词。
            while (iss >> word) {
                // 停用词承载的信息量低，不应进入推荐候选词典。
                if (enStopWords_.count(word) != 0) {
                    continue;
                }
                // operator[] 在 word 首次出现时插入值初始化为 0 的 int，
                // 前置 ++ 随后将其加 1；再次出现时直接累加已有词频。
                ++wordFrequency[word];
            }
        }
    }

    // 输出格式固定为：word frequency。词典不显式写行号，行号由记录在文件中的
    // 物理顺序隐含表示，并在 build_en_index 中从 1 开始重新计数。
    // 阶段 3：创建输出文件。ofstream 默认使用截断模式，旧词典会被完整覆盖。
    std::ofstream ofs(outfile);
    if (!ofs) {
        throw std::runtime_error("failed to open output dictionary: " + outfile);
    }

    // C++17 结构化绑定把 map 元素 pair<const string, int> 分别命名为
    // word 和 frequency；const auto& 避免复制键和值。
    for (const auto& [word, frequency] : wordFrequency) {
        ofs << word << ' ' << frequency << '\n';
    }

    std::cout << "[Keyword] English files: " << files.size()
              << ", dict size: " << wordFrequency.size() << std::endl;
}

/**
 * @brief 根据英文词典建立“字符到词典行号集合”的索引。
 * @param dict 英文词典输入路径。
 * @param index 英文索引输出路径。
 * @throws std::runtime_error 输入词典或输出索引无法打开时抛出。
 */
void KeywordProcessor::build_en_index(const std::string& dict, const std::string& index)
{
    // 阶段 1：重新读取磁盘词典，确保统计的行号就是二期实际加载的物理行号。
    // 索引来源是刚生成的词典而不是原始统计 map，确保索引行号与磁盘文件
    // 的真实记录顺序完全一致。
    std::ifstream ifs(dict);
    if (!ifs) {
        throw std::runtime_error("failed to open English dictionary: " + dict);
    }

    // char -> set<lineNo>。set 可以自动去重并保持行号有序。
    std::map<char, std::set<int>> charIndex;
    std::string word;
    int frequency = 0;
    int lineNo = 0;

    // 每次连续提取一个 string 和一个 int；任一字段读取失败都会结束循环。
    while (ifs >> word >> frequency) {
        // 约定词典行号从 1 开始；二期加载词典时必须使用相同约定。
        ++lineNo;

        // 一个单词中同一个字母可能出现多次，但索引中只需要记录一次该单词行号。
        // set 的迭代器区间构造会遍历单词字符并自动去重、排序。
        std::set<char> uniqueChars(word.begin(), word.end());
        for (char ch : uniqueChars) {
            // 嵌套容器表达 char -> set<lineNo>。外层 operator[] 首次访问 ch 时
            // 创建空 set，insert 再写入当前行号。
            charIndex[ch].insert(lineNo);
        }
    }

    // 阶段 2：覆盖写出索引文件。
    // 输出格式：character lineNo1 lineNo2 ...
    // 外层 map 保证字符有序，内层 set 保证行号有序且不重复。
    std::ofstream ofs(index);
    if (!ofs) {
        throw std::runtime_error("failed to open English index: " + index);
    }

    for (const auto& [ch, lines] : charIndex) {
        ofs << ch;
        // lines 已由 set 排序，按值遍历 int 没有复制开销问题。
        for (int no : lines) {
            ofs << ' ' << no;
        }
        ofs << '\n';
    }

    std::cout << "[Keyword] English index keys: " << charIndex.size() << std::endl;
}

/**
 * @brief 使用 Jieba 切分中文语料并生成纯汉字词频词典。
 * @param dir 中文语料目录。
 * @param outfile 中文词典输出路径。
 * @throws std::runtime_error 目录无法扫描、语料无法读取或输出无法打开时抛出。
 * @throws utf8::exception 分词结果编码非法时可能抛出。
 */
void KeywordProcessor::create_cn_dict(const std::string& dir, const std::string& outfile)
{
    // 阶段 1：扫描文件并准备跨文件累计的词频表。
    // 中文词典同样使用 map 保证字典序输出稳定，值保存整个语料中的累计词频。
    std::map<std::string, int> wordFrequency;
    auto files = DirectoryScanner::scan(dir);

    for (const auto& file : files) {
        std::ifstream ifs(file);
        if (!ifs) {
            throw std::runtime_error("failed to open Chinese corpus file: " + file);
        }

        // 中文分词可能依赖跨行上下文，这里一次读入整篇文件后统一交给 Jieba，
        // 而不是逐行切分。ostringstream 会保留原文件中的换行符。
        // rdbuf() 返回输入流底层缓冲区；插入 ostringstream 可一次复制文件
        // 剩余全部内容，随后通过 str() 取得 std::string。
        std::ostringstream buffer;
        buffer << ifs.rdbuf();
        std::string text = buffer.str();

        std::vector<std::string> words;
        // 未传 HMM 参数时使用 cppjieba 的 Mix 模式：先进行最大概率分词，
        // 再用 HMM 识别未登录词，与 PDF 推荐方式一致。
        tokenizer_.Cut(text, words);

        // 阶段 2：过滤 Jieba 结果并累计有效 token 词频。
        for (const auto& word : words) {
            // 先过滤停用词，再只保留完全由汉字组成的 token。英文推荐已有独立词典，
            // 若继续收录英文、圈号等内容，会污染中文字符索引和候选集合。
            if (cnStopWords_.count(word) != 0) {
                continue;
            }
            if (!TextUtils::is_chinese_word(word)) {
                continue;
            }
            ++wordFrequency[word];
        }
    }

    // 输出格式与英文词典一致：word frequency，每条记录占一行。
    // 阶段 3：覆盖写出格式统一的 `word frequency` 词典。
    std::ofstream ofs(outfile);
    if (!ofs) {
        throw std::runtime_error("failed to open output dictionary: " + outfile);
    }

    for (const auto& [word, frequency] : wordFrequency) {
        ofs << word << ' ' << frequency << '\n';
    }

    std::cout << "[Keyword] Chinese files: " << files.size()
              << ", dict size: " << wordFrequency.size() << std::endl;
}

/**
 * @brief 根据中文词典建立“Unicode 字符到词典行号集合”的索引。
 * @param dict 中文词典输入路径。
 * @param index 中文字符索引输出路径。
 * @throws std::runtime_error 输入词典或输出索引无法打开时抛出。
 * @throws utf8::exception 词典中存在非法 UTF-8 单词时抛出。
 */
void KeywordProcessor::build_cn_index(const std::string& dict, const std::string& index)
{
    // 阶段 1：按词典物理顺序读取记录并建立内存索引。
    // 中文索引同样依据最终磁盘词典构建，避免内存顺序与输出顺序不一致。
    std::ifstream ifs(dict);
    if (!ifs) {
        throw std::runtime_error("failed to open Chinese dictionary: " + dict);
    }

    // 中文字符是 UTF-8 多字节字符，key 必须用 string，不能用 char。
    std::map<std::string, std::set<int>> charIndex;
    std::string word;
    int frequency = 0;
    int lineNo = 0;

    while (ifs >> word >> frequency) {
        // 与英文索引统一，第一条词典记录的行号为 1。
        ++lineNo;

        // 先按 Unicode 字符拆分，再用 set 去掉词内重复字符。例如一个词中
        // 同一个汉字出现两次，该词典行号在对应字符索引中仍只保存一次。
        std::set<std::string> uniqueChars;
        // split_utf8_characters 返回临时 vector，其生命周期会延长到
        // 本次范围 for 循环结束；ch 通过常量引用绑定到其中的每个 UTF-8 字符串。
        for (const auto& ch : TextUtils::split_utf8_characters(word)) {
            // 正常生成的中文词典已经是纯汉字；此处再次校验，使函数读取外部或
            // 旧词典时也不会把英文字母、数字和特殊符号写入中文索引。
            if (TextUtils::is_chinese_word(ch)) {
                uniqueChars.insert(ch);
            }
        }

        for (const auto& ch : uniqueChars) {
            charIndex[ch].insert(lineNo);
        }
    }

    // 输出格式：UTF-8 character lineNo1 lineNo2 ...
    // character 使用 string 保存，不能使用只能容纳单字节的 char。
    // 阶段 2：覆盖写出 `character lineNo...` 格式索引。
    std::ofstream ofs(index);
    if (!ofs) {
        throw std::runtime_error("failed to open Chinese index: " + index);
    }

    for (const auto& [ch, lines] : charIndex) {
        ofs << ch;
        for (int no : lines) {
            ofs << ' ' << no;
        }
        ofs << '\n';
    }

    std::cout << "[Keyword] Chinese index keys: " << charIndex.size() << std::endl;
}
