#include "RabbitMqOssUploader.h"

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <stdexcept>
#include <vector>

using namespace std;
using json = nlohmann::json;
namespace amqp = AmqpClient;

/*
    读取可选环境变量。

    RabbitMQ 本地开发常用 guest/guest，所以这里提供默认值。
    如果实际部署的用户名、密码、端口不同，可以在 .env 中覆盖。
*/
static string getEnvOrDefault(const char* name, const string& default_value)
{
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        return default_value;
    }
    return string(value);
}

/*
    RabbitMQ 连接和路由配置。

    这组名字和 PDF 示例保持一致：
    - exchange：oss.direct
    - queue：oss.queue
    - routing key：oss
*/
static const string RabbitMqUri = getEnvOrDefault("RABBITMQ_URI", "amqp://guest:guest@localhost:5672/%2f");
static const string RabbitMqExchange = getEnvOrDefault("RABBITMQ_EXCHANGE", "oss.direct");
static const string RabbitMqQueue = getEnvOrDefault("RABBITMQ_QUEUE", "oss.queue");
static const string RabbitMqRoutingKey = getEnvOrDefault("RABBITMQ_ROUTING_KEY", "oss");

/*
    把二进制字符串编码成 Base64。

    RabbitMQ 消息体本身能放二进制，但我们想用 JSON 同时保存 uid、hashcode、content。
    JSON 字符串需要合法文本，所以先把文件内容转成 Base64。
*/
static string base64_encode(const string& input)
{
    if (input.empty()) {
        return "";
    }

    // Base64 编码后长度固定为 4 * ceil(n / 3)。
    int output_len = 4 * ((input.size() + 2) / 3);
    string output(output_len, '\0');

    /*
        OpenSSL 的 EVP_EncodeBlock 按 unsigned char* 处理字节。
        reinterpret_cast 只是改变指针视角，不会修改原始数据。
    */
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&output[0]),
                    reinterpret_cast<const unsigned char*>(input.data()),
                    input.size());
    return output;
}

/*
    把 Base64 字符串解码回原始文件内容。

    返回 false 表示消息内容损坏，消费者应该丢弃这条消息，而不是无限重试。
*/
static bool base64_decode(const string& input, string& output)
{
    if (input.empty()) {
        output.clear();
        return true;
    }

    // 每 4 个 Base64 字符最多还原 3 个原始字节。
    vector<unsigned char> buffer(input.size() / 4 * 3 + 3);
    int decoded_len = EVP_DecodeBlock(buffer.data(),
                                      reinterpret_cast<const unsigned char*>(input.data()),
                                      input.size());
    if (decoded_len < 0) {
        return false;
    }

    /*
        Base64 末尾的 '=' 是补齐符号。
        EVP_DecodeBlock 返回长度时会把补齐位也算进去，所以这里手动扣掉。
    */
    int padding = 0;
    if (!input.empty() && input[input.size() - 1] == '=') {
        ++padding;
    }
    if (input.size() >= 2 && input[input.size() - 2] == '=') {
        ++padding;
    }
    decoded_len -= padding;

    output.assign(reinterpret_cast<char*>(buffer.data()), decoded_len);
    return true;
}

/*
    创建 RabbitMQ Channel。

    Channel 可以理解为当前进程和 RabbitMQ 之间的一条 AMQP 通道。
    发布消息和消费消息都要先通过 Channel 连接到 RabbitMQ。
*/
static amqp::Channel::ptr_t create_rabbitmq_channel()
{
    return amqp::Channel::CreateFromUri(RabbitMqUri);
}

/*
    声明交换机、队列和绑定关系。

    RabbitMQ 的消息流向是：
        Producer -> Exchange -> Queue -> Consumer

    生产者把任务发给 exchange，exchange 根据 routing key 把任务投递给 queue。
*/
static void declare_rabbitmq_topology(const amqp::Channel::ptr_t& channel)
{
    /*
        direct 交换机要求 routing key 精确匹配。
        durable=true 表示 RabbitMQ 重启后交换机仍然存在。
    */
    channel->DeclareExchange(RabbitMqExchange,
                             amqp::Channel::EXCHANGE_TYPE_DIRECT,
                             false,
                             true,
                             false);

    /*
        durable=true 表示队列持久化。
        exclusive=false 表示不是当前连接独占，后续可以启动多个消费者。
        auto_delete=false 表示连接断开后队列不会自动删除。
    */
    channel->DeclareQueue(RabbitMqQueue,
                          false,
                          true,
                          false,
                          false);

    // 绑定后，routing key 为 oss 的消息会进入 oss.queue。
    channel->BindQueue(RabbitMqQueue, RabbitMqExchange, RabbitMqRoutingKey);
}

RabbitMqOssUploader::RabbitMqOssUploader(OssStorage& oss_storage)
    : oss_storage_(oss_storage)
    , stopping_(false)
{}

RabbitMqOssUploader::~RabbitMqOssUploader()
{
    // 析构时兜底停止线程，避免忘记手动调用 stop()。
    stop();
}

void RabbitMqOssUploader::start()
{
    /*
        如果线程已经启动，就不要重复启动。
        重复给同一个 std::thread 赋新线程会导致程序终止。
    */
    if (worker_.joinable()) {
        return;
    }

    stopping_ = false;
    worker_ = thread(&RabbitMqOssUploader::worker_loop, this);
}

void RabbitMqOssUploader::stop()
{
    // 通知后台线程退出循环。
    stopping_ = true;

    /*
        join() 会等待后台线程函数真正返回。
        这样可以保证 OssStorage 析构前，不再有后台线程使用 OSS SDK。
    */
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool RabbitMqOssUploader::publish(int uid, const string& hashcode, const string& content)
{
    try {
        amqp::Channel::ptr_t channel = create_rabbitmq_channel();
        declare_rabbitmq_topology(channel);

        json task;
        /*
            uid/hashcode 决定 OSS ObjectName：
                users/{uid}/{hashcode}

            contentBase64 保存真实文件内容。
            消费者收到后先解码，再调用 OssStorage 上传。
        */
        task["uid"] = uid;
        task["hashcode"] = hashcode;
        task["contentBase64"] = base64_encode(content);

        amqp::BasicMessage::ptr_t message = amqp::BasicMessage::Create(task.dump());
        message->ContentType("application/json");
        /*
            dm_persistent 表示消息持久化。
            它要配合 durable 队列使用，RabbitMQ 重启后消息才更有机会保留。
        */
        message->DeliveryMode(amqp::BasicMessage::dm_persistent);

        channel->BasicPublish(RabbitMqExchange, RabbitMqRoutingKey, message);
        cout << "[RabbitMQ publish] uid=" << uid << ", hashcode=" << hashcode << endl;
        return true;
    } catch (const exception& ex) {
        cerr << "[RabbitMQ publish FAILED] " << ex.what() << endl;
        return false;
    }
}

void RabbitMqOssUploader::worker_loop()
{
    /*
        外层循环负责断线重连。
        如果 RabbitMQ 临时不可用，捕获异常后稍等几秒，再重新连接。
    */
    while (!stopping_) {
        try {
            amqp::Channel::ptr_t channel = create_rabbitmq_channel();
            declare_rabbitmq_topology(channel);

            /*
                这里使用 BasicGet 拉取模式，而不是 BasicConsume 推送模式。

                原因：
                - 当前 SimpleAmqpClient 的推送消费会触发旧式 global_qos
                - 新版本 RabbitMQ 默认拒绝 global_qos，连接会被服务端关闭
                - 拉取模式不需要订阅 consumer，也就避开了这个兼容问题

                对学习项目来说，每秒轮询一次队列足够直观，也足够使用。
            */
            cout << "[RabbitMQ consumer] polling messages from " << RabbitMqQueue << endl;

            /*
                内层循环负责正常消费消息：
                1. BasicGet 主动从队列取一条消息
                2. 队列为空时返回 false，不会阻塞
                3. 没取到消息就 sleep 1 秒，避免空循环占满 CPU
            */
            while (!stopping_) {
                amqp::Envelope::ptr_t envelope;
                if (!channel->BasicGet(envelope, RabbitMqQueue, false)) {
                    this_thread::sleep_for(chrono::seconds(1));
                    continue;
                }

                if (!envelope || !envelope->Message()) {
                    continue;
                }

                const string& body = envelope->Message()->Body();
                json task = json::parse(body, nullptr, false);
                if (task.is_discarded()
                    || !task.contains("uid")
                    || !task["uid"].is_number_integer()
                    || !task.contains("hashcode")
                    || !task["hashcode"].is_string()
                    || !task.contains("contentBase64")
                    || !task["contentBase64"].is_string()) {
                    /*
                        消息格式坏了，重新入队也无法修复。
                        BasicReject(..., false) 表示丢弃这条消息。
                    */
                    cerr << "[RabbitMQ consume FAILED] invalid message body" << endl;
                    channel->BasicReject(envelope, false);
                    continue;
                }

                int uid = task["uid"].get<int>();
                string hashcode = task["hashcode"].get<string>();
                string content_base64 = task["contentBase64"].get<string>();
                string content;
                if (!base64_decode(content_base64, content)) {
                    cerr << "[RabbitMQ consume FAILED] invalid base64 content" << endl;
                    channel->BasicReject(envelope, false);
                    continue;
                }

                /*
                    真正耗时的 OSS PutObject 在后台线程中执行。
                    HTTP 上传接口只负责把任务投递到 RabbitMQ。
                */
                if (oss_storage_.upload_object(uid, hashcode, content)) {
                    channel->BasicAck(envelope);
                    cout << "[RabbitMQ consume OK] uid=" << uid << ", hashcode=" << hashcode << endl;
                } else {
                    /*
                        OSS 上传失败可能是临时网络或服务异常。
                        requeue=true 表示把消息放回队列，稍后继续重试。
                    */
                    cerr << "[RabbitMQ consume FAILED] OSS upload failed, requeue message" << endl;
                    channel->BasicReject(envelope, true);
                    this_thread::sleep_for(chrono::seconds(2));
                }
            }
        } catch (const exception& ex) {
            if (!stopping_) {
                cerr << "[RabbitMQ consumer ERROR] " << ex.what() << endl;
                this_thread::sleep_for(chrono::seconds(3));
            }
        }
    }
}
