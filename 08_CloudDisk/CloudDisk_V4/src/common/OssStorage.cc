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

static string getEnvOrThrow(const char* name)
{
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        throw runtime_error(string("Missing environment variable: ") + name);
    }
    return string(value);
}

static const string OssEndpoint = getEnvOrThrow("ALIBABA_CLOUD_OSS_ENDPOINT");
static const string OssAccessKeyId = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_ID");
static const string OssAccessKeySecret = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
static const string OssBucketName = getEnvOrThrow("ALIBABA_CLOUD_OSS_BUCKET");
static const string OssRegion = getEnvOrThrow("ALIBABA_CLOUD_OSS_REGION");

static unique_ptr<oss::OssClient> create_oss_client()
{
    oss::ClientConfiguration conf;

    auto client = make_unique<oss::OssClient>(
        OssEndpoint,
        OssAccessKeyId,
        OssAccessKeySecret,
        conf);

    client->SetRegion(OssRegion);
    return client;
}

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
    oss::InitializeSdk();
}

OssStorage::~OssStorage()
{
    oss::ShutdownSdk();
}

string OssStorage::object_name(int uid, const string& hashcode) const
{
    return "users/" + to_string(uid) + "/" + hashcode;
}

bool OssStorage::upload_object(int uid, const string& hashcode, const string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object = object_name(uid, hashcode);

    auto stream = make_shared<stringstream>(ios::in | ios::out | ios::binary);
    stream->write(content.data(), content.size());
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
    oss_content << stream->rdbuf();
    content = oss_content.str();
    return OssDownloadStatus::Ok;
}
