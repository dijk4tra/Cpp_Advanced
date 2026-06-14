#include <alibabacloud/oss/OssClient.h>

using namespace std;
using namespace AlibabaCloud::OSS;

int main(void)
{
    // 1. 初始化 OSS SDK
    InitializeSdk();

    // 2. 配置 OSS 访问参数
    string endpoint = "oss-cn-wuhan-lr.aliyuncs.com";
    string accessKeyId = "AccessKey ID";
    string accessKeySecret = "AccessKey Secret";
    string region = "cn-wuhan";

    ClientConfiguration conf;

    // 创建 OSS 客户端
    OssClient client(endpoint, accessKeyId, accessKeySecret, conf);

    // 设置 region
    client.SetRegion(region);

    // 3. 从本地文件上传到 OSS
    string bucketName = "peanutixx-oss-demo"; // bucket 名称
    string objectName = "dir/demo2.txt";      // OSS 对象路径

    // 直接传入文件路径：
    // SDK 会自动：
    // 1. 打开 a.txt
    // 2. 读取文件内容
    // 3. 上传到 OSS
    auto outcome = client.PutObject(bucketName, objectName, "a.txt");

    // 4. 判断上传结果
    if (!outcome.isSuccess()) {
        cout << "PutObject FAILED"
             << ", code:" << outcome.error().Code()
             << ", message:" << outcome.error().Message()
             << ", requestId:" << outcome.error().RequestId() << endl;

        exit(1);
    }

    // 5. 释放 SDK 资源
    ShutdownSdk();
    return 0;
}
