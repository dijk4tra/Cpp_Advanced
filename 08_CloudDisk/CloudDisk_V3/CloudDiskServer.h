#pragma once
#include "OssStorage.h"
#include "RabbitMqOssUploader.h"

#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

// 设计原则：组合优于继承
//     组合：有选择的复用代码
//     继承：会复用基类的所有代码，代码复用的技术！

// 装饰器模式（Wrapper）: 组合
//     保持接口一致，可以降低用户的学习成本

class CloudDiskServer {
public:
    CloudDiskServer();
    ~CloudDiskServer();

    // 注册路由
    void register_routes();

    // 包装了一层: 要保证包装后的接口与原来的接口一致！
    int start(unsigned short port)
    {
        return server_.start(port);
    }

    void stop() {
        server_.stop();
    }

    void list_routes() {
        server_.list_routes();
    }

private:
    // 注册路由
    void register_www_module();
    void register_auth_module();
    void register_user_module();
    void register_file_module();

private:
    wfrest::HttpServer server_; // 组合

    // OssStorage 只负责 OSS SDK 生命周期和对象上传/下载
    OssStorage oss_storage_;

    // RabbitMqOssUploader 只负责 RabbitMQ 任务发布和后台消费
    // 它需要借用 oss_storage_ 来执行真正的 OSS 上传
    RabbitMqOssUploader oss_uploader_;
};
