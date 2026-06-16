#pragma once
#include "../../rpc_gen/cloud_disk.srpc.h"
#include "../common/RabbitMqOssUploader.h"
#include "../common/ServiceRegistry.h"

#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

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
    wfrest::HttpServer server_;

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

    // 服务发现器。
    //
    // 第五期开始，API Gateway 不再长期保存固定地址的 srpc client。
    // 每次需要调用后端服务时，先通过 ServiceDiscovery 按服务名查一个健康实例，
    // 再用查到的 host/port 创建本次 RPC 调用需要的 client。
    ServiceDiscovery discovery_;
};
