#pragma once

#include <string>

// 下载接口需要区分对象不存在和 OSS 服务异常，以映射不同 HTTP 状态码。
enum class OssDownloadStatus {
    Ok,
    NotFound,
    Failed
};

/*
    OssStorage 专门负责和阿里云 OSS 交互。

    这个类只关心“对象存储”这一件事：
    - 初始化 OSS SDK
    - 释放 OSS SDK
    - 上传文件内容
    - 下载文件内容

    它不关心 HTTP 请求、MySQL、RabbitMQ。
*/
class OssStorage {
public:
    OssStorage();
    ~OssStorage();

    // 禁止拷贝，避免一个进程中出现多个对象重复管理 OSS SDK 全局生命周期。
    OssStorage(const OssStorage&) = delete;
    OssStorage& operator=(const OssStorage&) = delete;

    // 将文件内容 content 上传到 OSS，保存位置由 uid 和 hashcode 共同决定。
    bool upload_object(int uid, const std::string& hashcode, const std::string& content);

    // 从 OSS 下载文件内容；成功时 content 会被写入真实文件字节。
    OssDownloadStatus download_object(int uid,
                                      const std::string& hashcode,
                                      std::string& content);

private:
    // 生成 OSS ObjectName，例如 users/3/abc123。
    std::string object_name(int uid, const std::string& hashcode) const;
};
