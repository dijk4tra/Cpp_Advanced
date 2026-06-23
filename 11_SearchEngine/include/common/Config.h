#pragma once

#include <map>
#include <string>

/**
 * @brief 读取并保存 `key=value` 格式的项目配置。
 *
 * Config 将配置文件解析为内存中的键值映射，使离线处理模块只依赖配置项名称，
 * 不依赖语料和输出文件的实际目录。配置文件允许空行、行首和行尾空白，以及
 * 以 `#` 开头的整行注释；不支持行尾注释。
 *
 * 对象构造完成后内容保持不变，可以通过 get() 重复查询。解析失败时不会产生
 * 一个不完整的 Config 对象，而是直接抛出异常，由程序入口统一处理。
 */
class Config
{
public:
    /**
     * @brief 从指定文件加载全部配置项。
     *
     * 每个有效行必须包含一个非空 key 和一个非空 value，中间以第一个 `=`
     * 分隔。同名 key 出现多次时，后出现的值覆盖前面的值。
     *
     * @param filename 配置文件路径，可以是相对路径或绝对路径。
     * @throws std::runtime_error 文件无法打开，或某个有效行不符合
     *         `key=value` 格式时抛出。
     */
    explicit Config(const std::string& filename);

    /**
     * @brief 查询指定配置项的值。
     *
     * @param key 要查询的配置项名称。
     * @return 配置值的常量引用。引用的生命周期不超过当前 Config 对象。
     * @throws std::runtime_error key 不存在时抛出，避免调用者静默使用错误路径。
     */
    const std::string& get(const std::string& key) const;

private:
    // key -> value。std::map 使配置项按 key 有序保存，便于调试。
    std::map<std::string, std::string> items_;
};
