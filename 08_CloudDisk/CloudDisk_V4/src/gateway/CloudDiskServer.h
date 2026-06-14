#pragma once
#include "../../rpc_gen/cloud_disk.srpc.h"
#include "../common/RabbitMqOssUploader.h"

#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

class CloudDiskServer {
public:
    CloudDiskServer();
    ~CloudDiskServer();

    void register_routes();

    int start(unsigned short port)
    {
        return server_.start(port);
    }

    void stop() { server_.stop(); }

    void list_routes() { server_.list_routes(); }

private:
    void register_www_module();
    void register_auth_module();
    void register_user_module();
    void register_file_module();

private:
    wfrest::HttpServer server_;

    // 网关只发布上传任务；后台消费和 OSS 上传由 oss_upload_worker 负责。
    RabbitMqOssUploader oss_uploader_;

    cloud::disk::AuthService::SRPCClient auth_client_;
    cloud::disk::UserService::SRPCClient user_client_;
    cloud::disk::FileMetaService::SRPCClient filemeta_client_;
};
