#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <iostream>
#include <string>

using namespace std;
using namespace AmqpClient;

int main()
{
    // =========================================================
    // 1. 通过 URI 创建 RabbitMQ 连接 Channel
    // =========================================================
    // URI 格式：
    // amqp://用户名:密码@主机:端口/vhost
    string uri = "amqp://guest:guest@localhost:5672/%2f";
    Channel::ptr_t channel = Channel::CreateFromUri(uri);

    // =========================================================
    // 2. 消费消息（两种方式：拉取 / 推送）
    // =========================================================

    // -------------------------
    // 方式1：Pull（拉取模式）
    // -------------------------
    // 特点：
    // - 消费者主动从队列获取消息
    // - 非阻塞调用（没有消息会立即返回 false）
    // - 适合轮询场景

    /*
    const string& q = "oss.queue";

    // 用于接收消息的封装对象
    Envelope::ptr_t envelope;

    // 从队列中“尝试获取一条消息”
    // 成功：envelope 被填充
    // 失败：返回 false（队列为空）
    bool ok = channel->BasicGet(envelope, q);

    if (ok && envelope && envelope->Message()) {
        // 输出消息体内容
        cout << envelope->Message()->Body() << endl;
    }
    */

    // -------------------------
    // 方式2：Push（推送模式 / 订阅模式）
    // -------------------------
    // 特点：
    // - 消费者订阅队列
    // - RabbitMQ 主动推送消息
    // - 阻塞等待（适合实时消费）

    const string& q = "oss.queue";

    // 订阅队列（开启消费者）
    channel->BasicConsume(q);

    // 阻塞式等待 RabbitMQ 推送消息
    Envelope::ptr_t envelope = channel->BasicConsumeMessage();

    // 安全判断并输出消息内容
    if (envelope && envelope->Message()) {
        cout << envelope->Message()->Body() << endl;
    }

    return 0;
}
