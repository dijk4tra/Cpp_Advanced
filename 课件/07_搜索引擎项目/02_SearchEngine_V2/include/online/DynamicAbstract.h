#pragma once

#include <string>
#include <vector>

/**
 * @brief 根据查询关键词从网页正文生成动态摘要。
 *
 * 动态摘要会优先选择包含查询词、位置更重要且关键词更集中的正文窗口。位置
 * 权重按照文档开发思路规定：开头 1.30，结尾 1.15，中间 1.00。
 */
class DynamicAbstract
{
public:
    /**
     * @brief 生成动态摘要。
     *
     * @param content 原始网页正文，可能包含 HTML 标签。
     * @param keywords 查询关键词列表。
     * @param abstractLength 摘要字符数上限。
     * @return 带 <em> 高亮标签的摘要文本。
     */
    static std::string generate(const std::string& content,
                                const std::vector<std::string>& keywords,
                                int abstractLength);
};
