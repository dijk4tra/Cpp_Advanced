#pragma once
#include "../../rpc_gen/cloud_disk.srpc.h"
#include "../common/RabbitMqOssUploader.h"

#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

// 面向对象：将专业的事情交给专业的“人”去做
// 设计原则：武学心法
// 设计模式：招数

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

    void stop() { server_.stop(); }

    void list_routes() { server_.list_routes(); }

private:
    // 注册路由
    void register_www_module();
    void register_auth_module();
    void register_user_module();
    void register_file_module();

private:
    // 名字中最好不要带具体的实现细节
    // 方便以后修改具体的实现
    wfrest::HttpServer server_; // 组合

    // RabbitMqOssUploader 在网关进程中只负责：
    // 1. 保存上传临时文件
    // 2. 删除无用临时文件
    // 3. 发布 RabbitMQ 上传任务
    //
    // 第四期开始，后台消费和 OSS 上传由独立的 oss_upload_worker 进程负责，
    // 所以 CloudDiskServer 构造函数不再调用 oss_uploader_.start()。
    //
    // 注意：这里使用 RabbitMqOssUploader 的默认构造函数，不持有 OssStorage。
    // 网关只有下载接口需要 OSS；下载时在局部创建 OssStorage 即可。
    RabbitMqOssUploader oss_uploader_;

    // AuthService 的 srpc 客户端。
    // 网关通过它调用 Register/Login/VerifyToken。
    cloud::disk::AuthService::SRPCClient auth_client_;

    // UserService 的 srpc 客户端。
    // 网关通过它调用 GetUserProfile。
    cloud::disk::UserService::SRPCClient user_client_;

    // FileMetaService 的 srpc 客户端。
    // 网关通过它调用 ListFiles/CreateFile/GetFileForDownload。
    cloud::disk::FileMetaService::SRPCClient filemeta_client_;
};
