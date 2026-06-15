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
    /* 这个构造函数只接收一个参数（OssStorage&）。
       在 C++ 中，如果一个构造函数可以只用一个参数调用，它默认会成为一个转换构造函数（Conversion Constructor）。

       如果没有加 explicit，编译器会允许如下的隐式转换：
            void do_something(RabbitMqOssUploader uploader);
            OssStorage my_storage;
            // 隐式转换：编译器会自动调用 RabbitMqOssUploader(my_storage) 创建一个临时对象
            do_something(my_storage);

        加了 explicit 之后：
        上述隐式转换在编译时就会报错。必须显式地创建对象：
            OssStorage my_storage;
            // 必须这样显式调用
            RabbitMqOssUploader uploader(my_storage);
            do_something(uploader);
    */
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
