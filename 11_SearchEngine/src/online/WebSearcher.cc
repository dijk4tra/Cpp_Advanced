#include "../../include/online/WebSearcher.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

/**
 * @brief 构造网页搜索器并初始化 cppjieba。
 */
WebSearcher::WebSearcher()
    // tokenizer_ 是 cppjieba::Jieba 对象，构造时会加载分词词典。
    // 放在成员初始化列表中，保证对象创建时分词器就已经可用。
    : tokenizer_()
{
}

/**
 * @brief 加载网页搜索依赖的离线数据。
 */
void WebSearcher::load(const std::string& pages,
                       const std::string& offsets,
                       const std::string& invertIndex,
                       const std::string& stopWords)
{
    // 加载顺序：停用词用于查询分词过滤；网页库用于结果展示；
    // 倒排索引用于召回和打分。三者都在服务启动阶段一次性加载完成。
    // 启动阶段加载失败直接抛异常，避免服务器带着不完整索引继续运行。
    stopWords_ = TextUtils::load_stop_words(stopWords);
    pageLibrary_.load(pages, offsets);
    load_inverted_index(invertIndex);
}
