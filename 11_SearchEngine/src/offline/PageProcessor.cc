#include "../../include/offline/KeywordProcessor.h"

#include "../../include/common/DirectoryScanner.h"
#include "../../include/common/TextUtils.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

KeywordProcessor::KeywordProcessor(const std::string& enStopWordsFile,
                                   const std::string& cnStopWordsFile)
    // Jieba 构造时会加载分词词典，开销较大，因此作为成员只初始化一次。
    // 停用词也在构造阶段一次性读入内存，后续扫描大量 token 时直接查集合。
    : tokenizer_()
    , enStopWords_(TextUtils::load_stop_words(enStopWordsFile))
    , cnStopWords_(TextUtils::load_stop_words(cnStopWordsFile))
{}

void KeywordProcessor::process(const std::string &cnDir,
                               const std::string &enDir,
                               const std::string &cnDict,
                               const std::string &cnIndex,
                               const std::string &enDict,
                               const std::string &enIndex)
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

void KeywordProcessor::create_en_dict(const std::string& dir, const std::string& outfile)
{
    // map 同时承担词频统计和字典序排序。按 key 有序输出后，
    // 相同语料每次生成的词典行号都保持稳定，这对行号型索引非常重要。
    std::map<std::string, int> wordFrequency;
    auto files = DirectoryScanner::scan(dir);

    for (const auto& file : files) {
        // 任何语料文件无法读取都视为建库失败，避免只处理部分语料却生成
        // 看似完整的词典。
        std::ifstream ifs(file);
        if (!ifs) {
            throw std::runtime_error("failed to open English corpus file: " + file);
        }

        std::string line;
        while (std::getline(ifs, line)) {
            // 英文语料要求: 数字和标点替换为空格，只保留字母，并统一小写。
            std::string normalized = TextUtils::normalize_english_line(line);

            std::istringstream iss(normalized);
            std::string word;
            // 归一化已经把非字母替换为空格，因此流提取即可得到纯小写单词。
            while (iss >> word) {
                // 停用词承载的信息量低，不应进入推荐候选词典。
                if (enStopWords_.count(word) != 0) {
                    continue;
                }
                ++wordFrequency[word];
            }
        }
    }

    // 输出格式固定为：word frequency。词典不显式写行号，行号由记录在文件中
    // 的物理顺序隐含表示，并在 build_en_index 中从 1 开始重新计数。
    std::ofstream ofs(outfile);
    if (!ofs) {
        throw std::runtime_error("failed to open output dictionary: " + outfile);
    }

    for (const auto& [word, frequency] : wordFrequency) {
        ofs << word << ' ' << frequency << '\n';
    }

    std::cout << "[Keyword] English files: " << files.size()
              << ", dict size: " << wordFrequency.size() << std::endl;
}

void KeywordProcessor::build_en_index(const std::string& dict, const std::string& index)
{
    // 索引来源是刚生成的词典而不是原始统计 map，
    // 确保索引行号与磁盘文件的真实记录顺序完全一致。
    std::ifstream ifs(dict);
    if (!ifs) {
        throw std::runtime_error("failed to open English dictionary: " + dict);
    }

    // char -> set<lineNo>。set 可以自动去重并保持行号有序。
    std::map<char, std::set<int>> charIndex;
    std::string word;
    int frequency = 0;
    int lineNo = 0;

    while (ifs >> word >> frequency) {
        // 课程约定词典行号从 1 开始；二期加载词典时必须使用相同约定。
        ++lineNo;

        // 一个单词中同一个字母可能出现多次，但索引中只需要记录一次该单词行号。
        std::set<char> uniqueChars(word.begin(), word.end());
        for (char ch : uniqueChars) {
            charIndex[ch].insert(lineNo);
        }
    }

    // 输出格式：character lineNo1 lineNo2 ...
    // 外层 map 保证字符有序，内层 set 保证行号有序且不重复。
    std::ofstream ofs(index);
    if (!ofs) {
        throw std::runtime_error("failed to open English index: " + index);
    }

    for (const auto& [ch, lines] : charIndex) {
        ofs << ch;
        for (int no : lines) {
            ofs << ' ' << no;
        }
        ofs << '\n';
    }

    std::cout << "[Keyword] English index keys: " << charIndex.size() << std::endl;
}
