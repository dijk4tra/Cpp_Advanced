#pragma once

#include <cppjieba/Jieba.hpp>
#include <set>
#include <string>

// KeywordProcessor 负责“关键字推荐”的离线建库：
// 1. 从英文语料生成英文词典 dict_en.dat。
// 2. 从英文词典生成英文索引 index_en.dat。
// 3. 从中文语料生成中文词典 dict_cn.dat。
// 4. 从中文词典生成中文索引 index_cn.dat。
class KeywordProcessor
{
public:
    KeywordProcessor(const std::string& enStopWordsFile,
                     const std::string& cnStopWordsFile);

    void process(const std::string& cnDir,
                 const std::string& enDir,
                 const std::string& cnDict,
                 const std::string& cnIndex,
                 const std::string& enDict,
                 const std::string& enIndex);

private:
    void create_cn_dict(const std::string& dir, const std::string& outfile);
    void build_cn_index(const std::string& dict, const std::string& index);

    void create_en_dict(const std::string& dir, const std::string& outfile);
    void build_en_index(const std::string& dict, const std::string& index);

private:
    // Jieba 初始化会加载词典文件，比较耗时，所以整个 Processor 只创建一个对象。
    cppjieba::Jieba tokenizer_;
    std::set<std::string> enStopWords_;
    std::set<std::string> cnStopWords_;
};
