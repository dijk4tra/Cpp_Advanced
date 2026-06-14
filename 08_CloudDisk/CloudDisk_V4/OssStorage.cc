#include "OssStorage.h"

#include <alibabacloud/oss/OssClient.h>
#include <alibabacloud/oss/client/ClientConfiguration.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace std;
namespace oss = AlibabaCloud::OSS;

// 读取必需环境变量
static string getEnvOrThrow(const char* name)
{
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        throw runtime_error(string("Missing environment variable: ") + name);
    }
    return string(value);
}

// OSS 连接配置
static const string OssEndpoint = getEnvOrThrow("ALIBABA_CLOUD_OSS_ENDPOINT");
static const string OssAccessKeyId = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_ID");
static const string OssAccessKeySecret = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
static const string OssBucketName = getEnvOrThrow("ALIBABA_CLOUD_OSS_BUCKET");
static const string OssRegion = getEnvOrThrow("ALIBABA_CLOUD_OSS_REGION");

/*
    创建 OSS 客户端。

    每次上传/下载都创建一个临时 OssClient：
    - 代码简单，生命周期清楚
    - 不需要确认 OssClient 是否能被多线程安全共享
    - 当前是学习项目，请求量不大，这个成本可以接受
*/
static unique_ptr<oss::OssClient> create_oss_client()
{
    oss::ClientConfiguration conf;

    /*
        make_unique 会创建 OssClient，并把它交给 unique_ptr 管理。
        这样函数返回后，调用者不需要手动 delete。
    */
    auto client = make_unique<oss::OssClient>(
        OssEndpoint,
        OssAccessKeyId,
        OssAccessKeySecret,
        conf);

    /*
        SetRegion 告诉 SDK 当前 Bucket 所在地域。
        endpoint 和 region 要匹配，否则 OSS 可能返回签名或地域相关错误。
    */
    client->SetRegion(OssRegion);
    return client;
}

/*
    统一打印 OSS 错误。

    HTTP 接口不应该把 Bucket、AccessKey、RequestId 等内部细节返回给前端；
    但服务端日志需要保留这些信息，方便排查权限、地域、网络问题。
*/
static void log_oss_error(const string& action, const oss::OssError& error)
{
    cerr << "[OSS " << action << " FAILED]"
         << " code:" << error.Code()
         << ", message:" << error.Message()
         << ", requestId:" << error.RequestId()
         << endl;
}

OssStorage::OssStorage()
{
    /*
        OSS C++ SDK 要求 InitializeSdk() 在使用任何 OSS API 前调用。
        CloudDiskServer 中只创建一个 OssStorage，所以这里也只初始化一次。
    */
    oss::InitializeSdk();
}

OssStorage::~OssStorage()
{
    /*
        ShutdownSdk() 释放 OSS SDK 内部的全局资源。
        RabbitMqOssUploader 停止后台线程后，OssStorage 才会析构，因此不会一边释放 SDK 一边上传文件。
    */
    oss::ShutdownSdk();
}

string OssStorage::object_name(int uid, const string& hashcode) const
{
    /*
        OSS 没有真正的目录，users/3/abc123 只是对象名字符串。
        按 uid 分前缀有两个好处：
        1. 不同用户上传相同 hash 的文件不会互相覆盖
        2. 后续按用户清理文件时，可以按 users/{uid}/ 前缀查找
    */
    return "users/" + to_string(uid) + "/" + hashcode;
}

bool OssStorage::upload_object(int uid, const string& hashcode, const string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object = object_name(uid, hashcode);

    /*
        OSS SDK 的 PutObjectRequest 需要 shared_ptr<iostream> 表示上传内容。
        stringstream 可以把内存中的 string 包装成一个“像文件一样可读取”的流。
    */
    auto stream = make_shared<stringstream>(ios::in | ios::out | ios::binary);
    stream->write(content.data(), content.size());

    /*
        write() 写完后，流的位置在末尾。
        seekg(0) 把读取位置移动回开头，否则 SDK 可能读不到完整内容。
    */
    stream->seekg(0);

    oss::PutObjectRequest request(bucket_name, object, stream);
    auto outcome = client->PutObject(request);
    if (!outcome.isSuccess()) {
        log_oss_error("PutObject", outcome.error());
        return false;
    }

    return true;
}

OssDownloadStatus OssStorage::download_object(int uid,
                                              const string& hashcode,
                                              string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object = object_name(uid, hashcode);

    auto outcome = client->GetObject(bucket_name, object);
    if (!outcome.isSuccess()) {
        if (outcome.error().Code() == "NoSuchKey") {
            return OssDownloadStatus::NotFound;
        }
        log_oss_error("GetObject", outcome.error());
        return OssDownloadStatus::Failed;
    }

    auto stream = outcome.result().Content();
    if (!stream) {
        cerr << "[OSS GetObject FAILED] empty content stream" << endl;
        return OssDownloadStatus::Failed;
    }

    ostringstream oss_content;
    /*
        stream->rdbuf() 可以拿到 OSS 返回内容流背后的缓冲区。
        这行代码会把 OSS 对象剩余的所有字节复制到 oss_content 中。
    */
    oss_content << stream->rdbuf();
    content = oss_content.str();
    return OssDownloadStatus::Ok;
}
