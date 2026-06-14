#pragma once

#include "OssStorage.h"

#include <atomic>
#include <string>
#include <thread>

// 封装 OSS 异步上传任务的临时文件、RabbitMQ 发布和后台消费流程。
class RabbitMqOssUploader {
public:
    // API Gateway 使用默认构造：只保存临时文件、清理临时文件和发布消息。
    RabbitMqOssUploader();

    // oss_upload_worker 传入 OssStorage 后可启动消费者线程执行上传。
    explicit RabbitMqOssUploader(OssStorage& oss_storage);
    ~RabbitMqOssUploader();

    RabbitMqOssUploader(const RabbitMqOssUploader&) = delete;
    RabbitMqOssUploader& operator=(const RabbitMqOssUploader&) = delete;

    void start();

    void stop();

    bool save_temp_file(int uid,
                        const std::string& hashcode,
                        const std::string& content,
                        std::string& temp_path);

    void remove_temp_file(const std::string& temp_path);

    bool publish(int uid, const std::string& hashcode, const std::string& temp_path);

private:
    void worker_loop();

private:
    // API Gateway 不消费消息，因此这里可以是 nullptr。
    OssStorage* oss_storage_;

    std::atomic<bool> stopping_;
    std::thread worker_;
};
