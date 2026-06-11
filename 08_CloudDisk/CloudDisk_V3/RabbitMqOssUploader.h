#pragma once

#include "OssStorage.h"

#include <atomic>
#include <string>
#include <thread>

/*
    RabbitMqOssUploader 专门负责“异步 OSS 上传任务”。

    它的职责包括：
    - 发布上传任务到 RabbitMQ
    - 启动后台消费者线程
    - 从队列中取任务
    - 调用 OssStorage 把文件真正上传到 OSS

    它不关心 HTTP 路由，也不关心 MySQL 表结构。
*/
class RabbitMqOssUploader {
public:
    /*
        传入 OssStorage&，表示 RabbitMqOssUploader 不拥有 OSS 存储对象。
        它只是“借用”这个对象来执行上传。
    */
    explicit RabbitMqOssUploader(OssStorage& oss_storage);
    ~RabbitMqOssUploader();

    // 禁止拷贝，避免复制线程对象和共享停止标志。
    RabbitMqOssUploader(const RabbitMqOssUploader&) = delete;
    RabbitMqOssUploader& operator=(const RabbitMqOssUploader&) = delete;

    // 启动后台消费者线程。
    void start();

    // 停止后台消费者线程，并等待它退出。
    void stop();

    // 发布一条 OSS 上传任务；成功交给 RabbitMQ 返回 true。
    bool publish(int uid, const std::string& hashcode, const std::string& content);

private:
    // 后台线程入口函数：循环从 RabbitMQ 队列消费消息。
    void worker_loop();

private:
    // 保存外部传进来的 OSS 存储对象引用，用于消费消息后上传文件。
    OssStorage& oss_storage_;

    // 主线程设置 stopping_=true 后，后台线程会尽快退出循环。
    std::atomic<bool> stopping_;

    // 后台消费者线程对象。
    std::thread worker_;
};
