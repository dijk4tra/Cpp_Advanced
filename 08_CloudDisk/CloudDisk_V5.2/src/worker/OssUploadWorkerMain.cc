#include "../common/OssStorage.h"
#include "../common/RabbitMqOssUploader.h"

#include <csignal>
#include <iostream>
#include <workflow/WFFacilities.h>

using namespace std;

/*
    OssUploadWorker 是第四期从 API Gateway 中拆出来的后台进程。

    它不是 srpc服务，也不监听端口。
    它只做一件事：
    - 从 RabbitMQ 消费上传任务，然后把临时文件上传到 OSS。
*/

// 主线程等待器。
// 初始值为 1，表示 worker 进程还需要继续运行。
static WFFacilities::WaitGroup wait_group(1);

// Ctrl+C 信号处理函数。
static void sig_handler(int)
{
    // 收到退出信号后，让 main() 中的 wait_group.wait() 返回。
    wait_group.done();
}

int main()
{
    // 注册 Ctrl+C 信号。
    signal(SIGINT, sig_handler);

    // OssStorage 负责 OSS SDK 生命周期。
    // 构造函数会调用 OSS SDK 的 InitializeSdk()。
    OssStorage oss_storage;

    /*
        RabbitMqOssUploader 复用第三期已有类。
        第四期中：
        - API Gateway 只使用它的 save_temp_file/remove_temp_file/publish。
        - OssUploadWorker 使用它的 start/stop 启动消费者线程。
    */
    RabbitMqOssUploader uploader(oss_storage);

    // 启动后台消费者线程。
    // 线程内部会连接 RabbitMQ、订阅队列、消费任务、上传 OSS。
    uploader.start();

    // 打印启动日志，便于确认 worker 已经独立运行。
    cout << "[OssUploadWorker] started" << endl;

    // 主线程等待 Ctrl+C。
    wait_group.wait();

    // 收到退出信号后，先停止消费者线程。
    // stop() 会等待线程退出，避免 OSS SDK 释放时后台线程还在上传。
    uploader.stop();

    // worker 正常结束。
    cout << "[OssUploadWorker] stopped" << endl;

    // main 返回后，oss_storage 析构，释放 OSS SDK。
    return 0;
}
