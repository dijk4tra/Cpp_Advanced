#include <SimpleAmqpClient/BasicMessage.h>
#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <string>

using namespace std;
using namespace AmqpClient;

int main()
{
    // =========================================================
    // 1. 创建 RabbitMQ 连接 Channel
    // =========================================================
    // 通过 host/port/用户名/密码/vhost 方式建立连接
    string host = "127.0.0.1";
    int port = 5672;          // AMQP 默认端口
    string username = "guest";
    string password = "guest";
    string vhost = "/";

    Channel::ptr_t channel =
        Channel::Create(host, port, username, password, vhost);

    // =========================================================
    // 2. 构建消息体
    // =========================================================
    // BasicMessage 用于封装要发送的消息内容
    BasicMessage::ptr_t message =
        BasicMessage::Create("Hello RabbitMQ");

    // =========================================================
    // 3. 发送消息（Publish）
    // =========================================================
    // exchange   : 交换机名称（决定消息如何路由）
    // routingKey : 路由键（匹配队列绑定规则）
    // message    : 实际发送的消息内容

    string exchange = "oss.direct";
    string routingKey = "oss";

    channel->BasicPublish(exchange, routingKey, message);

    return 0;
}
