#include <alibabacloud/oss/OssClient.h>

using namespace std;
using namespace AlibabaCloud::OSS;

int main(void)
{
    // 1. 初始化 OSS SDK（必须调用）
    // 用于初始化网络、线程池等底层资源
    InitializeSdk();

    // 2. 配置 OSS 访问参数
    string endpoint = "oss-cn-wuhan-lr.aliyuncs.com";    // OSS 访问域名
    string accessKeyId = "AccessKey ID";                 // AccessKey ID
    string accessKeySecret = "AccessKey Secret";         // AccessKey Secret
    string region = "cn-wuhan";                          // 地域

    // ClientConfiguration 用于配置超时、重试策略等（这里使用默认配置）
    ClientConfiguration conf;

    // 创建 OSS 客户端对象（所有操作都通过 client 完成）
    OssClient client(endpoint, accessKeyId, accessKeySecret, conf);

    // 设置 region（用于路由优化、请求签名等）
    client.SetRegion(region);

    // 3. 构造上传内容（内存数据上传）
    string bucketName = "peanutixx-oss-demo"; // 存储空间名称
    string objectName = "dir/demo1.txt";      // OSS 上的文件路径（对象名）

    // 要上传的内容（在内存中生成）
    string content = "Hello AlibbaCloud OSS";

    // 将 string 包装成输入流（stringstream）
    // OSS SDK 通过 stream 读取数据进行上传
    shared_ptr<iostream> stream =
        make_shared<stringstream>(std::move(content));
    /*
    // 1. 创建一个 stringstream：
    //    - stringstream 是基于内存的流
    //    - 它可以像文件一样被读取（逐字节）
    auto stringStream = make_shared<stringstream>(std::move(content));

    // 2. 向上转型为 iostream 指针：
    //    - OSS SDK 使用 iostream 作为统一接口
    //    - iostream 是 istream/ostream 的基类
    //    - 允许 SDK 统一读取数据来源（文件/内存/网络流）
    shared_ptr<iostream> stream = stringStream;
    */

    // 构造上传请求（使用流作为数据源）
    PutObjectRequest request(bucketName, objectName, stream);

    // 发起上传请求
    auto outcome = client.PutObject(request);

    // 4. 判断上传是否成功
    if (!outcome.isSuccess()) {
        // 输出错误信息（错误码、错误描述、请求ID）
        cout << "PutObject FAILED"
             << ", code:" << outcome.error().Code()
             << ", message:" << outcome.error().Message()
             << ", requestId:" << outcome.error().RequestId() << endl;

        // 失败直接退出
        exit(1);
    }

    // 5. 释放 SDK 资源（关闭网络线程等）
    ShutdownSdk();
    return 0;
}
