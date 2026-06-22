#include "../../include/offline/KeywordProcessor.h"

#include "../../include/common/DirectoryScanner.h"
#include "../../include/common/TextUtils.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

KeywordProcessor::KeywordProcessor(const std::string& enStopWordsFile,
                                   const std::string& cnStopWordsFile)
    : tokenizer_()
    , enStopWords_(TextUtils::load_stop_words(enStopWordsFile))
    , cnStopWords_(TextUtils::load_stop_words(cnStopWordsFile))
{
}

void KeywordProcessor::process(const std::string& cnDir,
                               const std::string& enDir,
                               const std::string& cnDict,
                               const std::string& cnIndex,
                               const std::string& enDict,
                               const std::string& enIndex)
{
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
    std::map<std::string, int> wordFrequency;
    auto files = DirectoryScanner::scan(dir);

    for (const auto& file : files) {
        std::ifstream ifs(file);
        if (!ifs) {
            throw std::runtime_error("failed to open English corpus file: " + file);
        }

        std::string line;
        while (std::getline(ifs, line)) {
            // 英文语料要求：数字和标点替换为空格，只保留字母，并统一小写。
            std::string normalized = TextUtils::normalize_english_line(line);

            std::istringstream iss(normalized);
            std::string word;
            while (iss >> word) {
                if (enStopWords_.count(word) != 0) {
                    continue;
                }
                ++wordFrequency[word];
            }
        }
    }

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
        ++lineNo;

        // 一个单词中同一个字母可能出现多次，但索引中只需要记录一次该单词行号。
        std::set<char> uniqueChars(word.begin(), word.end());
        for (char ch : uniqueChars) {
            charIndex[ch].insert(lineNo);
        }
    }

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

void KeywordProcessor::create_cn_dict(const std::string& dir, const std::string& outfile)
{
    std::map<std::string, int> wordFrequency;
    auto files = DirectoryScanner::scan(dir);

    for (const auto& file : files) {
        std::ifstream ifs(file);
        if (!ifs) {
            throw std::runtime_error("failed to open Chinese corpus file: " + file);
        }

        std::ostringstream buffer;
        buffer << ifs.rdbuf();
        std::string text = buffer.str();

        std::vector<std::string> words;
        tokenizer_.Cut(text, words); // 默认 Mix 模式，适合课程项目中的中文分词。

        for (const auto& word : words) {
            if (cnStopWords_.count(word) != 0) {
                continue;
            }
            if (TextUtils::is_useless_token(word)) {
                continue;
            }
            ++wordFrequency[word];
        }
    }

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

void KeywordProcessor::build_cn_index(const std::string& dict, const std::string& index)
{
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
        ++lineNo;

        std::set<std::string> uniqueChars;
        for (const auto& ch : TextUtils::split_utf8_characters(word)) {
            if (!TextUtils::is_useless_token(ch)) {
                uniqueChars.insert(ch);
            }
        }

        for (const auto& ch : uniqueChars) {
            charIndex[ch].insert(lineNo);
        }
    }

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
