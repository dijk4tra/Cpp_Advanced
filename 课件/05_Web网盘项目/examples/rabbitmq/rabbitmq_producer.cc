#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <string>

using namespace std;
using namespace AmqpClient;

int main()
{
    // 1. 创建Channel
    string host = "127.0.0.1";
    int port = 5672;
    string username = "guest";
    string password = "guest";
    string vhost = "/";
    Channel::ptr_t channel = Channel::Create(host, port, username, password, vhost);
    // 2. 构建消息
    BasicMessage::ptr_t message = BasicMessage::Create("Hello RabbitMQ");
    // 3. 发送消息
    string exchange = "oss.direct"; // 交换机
    string routingKey = "oss"; // 消息的routingKey
    channel->BasicPublish(exchange, routingKey, message); // 发布消息
}
