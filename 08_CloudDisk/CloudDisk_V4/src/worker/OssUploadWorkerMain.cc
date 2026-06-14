#include "../common/OssStorage.h"
#include "../common/RabbitMqOssUploader.h"

#include <csignal>
#include <iostream>
#include <workflow/WFFacilities.h>

using namespace std;

// 独立上传进程：从 RabbitMQ 消费任务并上传临时文件到 OSS。
static WFFacilities::WaitGroup wait_group(1);

static void sig_handler(int)
{
    wait_group.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    OssStorage oss_storage;

    RabbitMqOssUploader uploader(oss_storage);
    uploader.start();

    cout << "[OssUploadWorker] started" << endl;

    wait_group.wait();

    uploader.stop();

    cout << "[OssUploadWorker] stopped" << endl;
    return 0;
}
