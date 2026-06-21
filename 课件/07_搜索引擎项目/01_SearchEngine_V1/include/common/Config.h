#pragma once

#include <map>
#include <string>

// Config 负责读取 conf/config.conf 中的 key=value 配置。
//
// 这样做的好处是：代码不关心目录怎么摆放，只关心配置项名字。
// 后续如果移动语料、词典或索引文件，只需要修改配置文件。
class Config
{
public:
    explicit Config(const std::string& filename);

    // 获取指定 key 对应的 value。key 不存在时抛异常，避免静默使用错误路径。
    const std::string& get(const std::string& key) const;

private:
    std::map<std::string, std::string> items_;
};
