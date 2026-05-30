#include <alibabacloud/oss/OssClient.h>

using namespace std;
using namespace AlibabaCloud::OSS;

int main(void)
{
    // 1. 初始化网络等资源
    InitializeSdk();
    // 2. 设置OSS账号信息，创建OssClient对象
    string endpoint = "oss-cn-wuhan-lr.aliyuncs.com";
    string accessKeyId = "ALIBABA_CLOUD_ACCESS_KEY_ID";
    string accessKeySercret = "ALIBABA_CLOUD_ACCESS_KEY_SECRET";
    string region = "cn-wuhan";
    ClientConfiguration conf;
    OssClient client(endpoint, accessKeyId, accessKeySercret, conf);
    client.SetRegion(region);
    // 3. 上传文件
    string bucketName = "peanutixx-oss-demo";
    string objectName = "dir/demo2.txt";
    auto outcome = client.PutObject(bucketName, objectName, "a.txt");
    // 4. 处理错误
    if (!outcome.isSuccess()) {
        cout << "PutObject FAILED"
             << ", code:" << outcome.error().Code()
             << ", message:" << outcome.error().Message()
             << ", requestId:" << outcome.error().RequestId() << endl;
        exit(1);
    }
    // 5. 释放网络等资源
    ShutdownSdk();
    return 0;
}
