#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <iostream>
#include <string>

using namespace std;
using namespace AmqpClient;

int main()
{
    // 1. 以URI的方式创建Channel
    string uri = "amqp://guest:guest@localhost:5672/%2f";
    Channel::ptr_t channel = Channel::CreateFromUri(uri);

    // 2. 获取消息
    // 方式1：拉取模式 --- 消费者主动从队列中拉取消息（非阻塞式）
    // const string& q = "oss.queue";

    // 如果队列中有消息，将消息放入到evelope中，并返回true
    // 如果队列中没有消息，BasicGet会立刻返回false
    // Envelope::ptr_t envelope;
    // channel->BasicGet(envelope, q);
    // if (envelope && envelope->Message()) { */
    //     cout << envelope->Message()->Body() << endl;
    // }

    // 方式2：推送模式 --- 等待RabbitMQ推送消息 (阻塞式)
    const string& q = "oss.queue";
    channel->BasicConsume(q); // 订阅队列
    // 阻塞：等待RabbitMQ推送消息
    Envelope::ptr_t envelope = channel->BasicConsumeMessage();
    // 打印消息
    if (envelope && envelope->Message()) {
        cout << envelope->Message()->Body() << endl;
    }
}
