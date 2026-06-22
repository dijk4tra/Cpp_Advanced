#pragma once

#include <map>
#include <string>

// Config 复制读取 conf/config.conf 中的 key=value 配置
class Config
{
public:
    explicit Config(const std::string& filename);

    // 获取指定 key 对应的 value。key 不存在时抛异常，避免静默使用错误路径。
    const std::string& get(const std::string& key) const;

private:
    std::map<std::string, std::string> items_;
};
