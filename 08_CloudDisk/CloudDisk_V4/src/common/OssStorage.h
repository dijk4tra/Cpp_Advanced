#pragma once

#include <string>

// 下载接口需要区分对象不存在和 OSS 服务异常，以映射不同 HTTP 状态码。
enum class OssDownloadStatus {
    Ok,
    NotFound,
    Failed
};

// 封装阿里云 OSS SDK 生命周期和对象上传/下载。
class OssStorage {
public:
    OssStorage();
    ~OssStorage();

    // 避免多个对象重复管理 OSS SDK 全局生命周期。
    OssStorage(const OssStorage&) = delete;
    OssStorage& operator=(const OssStorage&) = delete;

    bool upload_object(int uid, const std::string& hashcode, const std::string& content);

    OssDownloadStatus download_object(int uid,
                                      const std::string& hashcode,
                                      std::string& content);

private:
    std::string object_name(int uid, const std::string& hashcode) const;
};
