#pragma once

#include <cppjieba/Jieba.hpp>
#include <set>
#include <string>

/**
 * @brief 构建关键字推荐所需的中英文词典库和字符索引库。
 *
 * 处理器完成四项离线任务：英文词频统计、英文字符索引、中文词频统计、中文
 * 字符索引。词典记录格式为 `word frequency`；索引记录格式为
 * `character lineNo...`，其中 lineNo 从 1 开始，对应词典文件中的物理行号。
 *
 * 对象持有一个长期复用的 Jieba 分词器和两套停用词，避免每个语料文件重复
 * 加载词典。该类不是线程安全的，不应在多个线程中同时调用 process()。
 */
class KeywordProcessor
{
public:
    /**
     * @brief 初始化分词器并加载中英文停用词。
     *
     * @param enStopWordsFile 英文停用词文件路径。
     * @param cnStopWordsFile 中文停用词文件路径。
     * @throws std::runtime_error 任一停用词文件无法打开时抛出。
     * @throws cppjieba 初始化分词词典失败时产生的异常会自然传播。
     */
    KeywordProcessor(const std::string& enStopWordsFile,
                     const std::string& cnStopWordsFile);

    /**
     * @brief 按依赖顺序生成全部关键字推荐离线数据。
     *
     * 每种语言都先生成词典，再根据词典物理行号生成字符索引。调用者需要提前
     * 创建输出文件的父目录。
     *
     * @param cnDir 中文语料目录。
     * @param enDir 英文语料目录。
     * @param cnDict 中文词典输出路径。
     * @param cnIndex 中文字符索引输出路径。
     * @param enDict 英文词典输出路径。
     * @param enIndex 英文字符索引输出路径。
     * @throws std::runtime_error 输入目录无法扫描、语料无法读取或输出无法创建时抛出。
     * @throws utf8::exception 中文语料包含非法 UTF-8 序列时可能抛出。
     */
    void process(const std::string& cnDir,
                 const std::string& enDir,
                 const std::string& cnDict,
                 const std::string& cnIndex,
                 const std::string& enDict,
                 const std::string& enIndex);

private:
    /**
     * @brief 对中文语料分词、过滤并统计词频，生成中文词典。
     * @param dir 中文语料目录。
     * @param outfile 中文词典输出路径。
     * @throws std::runtime_error 目录、输入文件或输出文件不可用时抛出。
     * @throws utf8::exception 分词结果包含非法 UTF-8 序列时可能抛出。
     */
    void create_cn_dict(const std::string& dir, const std::string& outfile);

    /**
     * @brief 将中文词典中的每个 Unicode 字符映射到包含它的词典行号。
     * @param dict 中文词典输入路径。
     * @param index 中文字符索引输出路径。
     * @throws std::runtime_error 输入词典或输出索引无法打开时抛出。
     * @throws utf8::exception 词典单词包含非法 UTF-8 序列时抛出。
     */
    void build_cn_index(const std::string& dict, const std::string& index);

    /**
     * @brief 清洗英文语料、过滤停用词并统计词频，生成英文词典。
     * @param dir 英文语料目录。
     * @param outfile 英文词典输出路径。
     * @throws std::runtime_error 目录、输入文件或输出文件不可用时抛出。
     */
    void create_en_dict(const std::string& dir, const std::string& outfile);

    /**
     * @brief 将每个英文字母映射到包含它的词典行号。
     * @param dict 英文词典输入路径。
     * @param index 英文字符索引输出路径。
     * @throws std::runtime_error 输入词典或输出索引无法打开时抛出。
     */
    void build_en_index(const std::string& dict, const std::string& index);

private:
    // 中文分词器。Jieba 初始化会加载词典，开销较大，因此整个对象只保留一份。
    cppjieba::Jieba tokenizer_;

    // 英文停用词集合，用于生成英文词典时过滤低信息量词语。
    std::set<std::string> enStopWords_;

    // 中文停用词集合，用于生成中文词典时过滤低信息量词语。
    std::set<std::string> cnStopWords_;
};
