#include "RabbitMqOssUploader.h"

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

using namespace std;
using json = nlohmann::json;
namespace amqp = AmqpClient;
namespace fs = std::filesystem;

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
    本地临时文件目录。

    第三期之前为了教学直观，RabbitMQ 消息体直接放文件内容。
    现在改成更接近生产的方式：
    1. HTTP 上传接口先把文件内容写到本地临时文件
    2. RabbitMQ 消息只保存 tempPath
    3. 消费者按 tempPath 读取文件，再上传到 OSS

    如果 .env 中没有配置 CLOUDDISK_TEMP_DIR，就默认写到 ./tmp/uploads。
    这个路径是相对 CloudDisk 程序运行目录的。
*/
static const string TempUploadDir = getEnvOrDefault("CLOUDDISK_TEMP_DIR", "./tmp/uploads");

/*
    生成临时文件路径。

    文件名包含 uid、hashcode 和当前时间戳：
        uid-hashcode-timestamp.tmp

    加时间戳是为了避免同一个用户连续上传同一个文件时，临时文件互相覆盖。
*/
static fs::path make_temp_file_path(int uid, const string& hashcode)
{
    /*
        system_clock::now() 获取当前时间。
        time_since_epoch() 表示从 1970-01-01 到现在经过的时间。
        nanoseconds 让文件名足够细，降低同一瞬间重复的概率。
    */
    auto now = chrono::system_clock::now().time_since_epoch();
    auto timestamp = chrono::duration_cast<chrono::nanoseconds>(now).count();
    string temp_filename = to_string(uid) + "-" + hashcode + "-" + to_string(timestamp) + ".tmp";
    return fs::path(TempUploadDir) / temp_filename;
}

/*
    从本地临时文件读取完整内容。

    消费者拿到 RabbitMQ 消息后，只能得到 tempPath。
    所以真正上传 OSS 之前，要先把这个临时文件重新读回内存。
*/
static bool read_temp_file(const string& temp_path, string& content)
{
    ifstream ifs(temp_path, ios::binary);
    if (!ifs) {
        cerr << "[TempFile read FAILED] open failed: " << temp_path << endl;
        return false;
    }

    ostringstream oss;
    oss << ifs.rdbuf();
    content = oss.str();
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
        EXCHANGE_TYPE_DIRECT 交换机要求 routing key 精确匹配。
        passive=false 表示如果交换机不存在，就创建它。
        durable=true 表示交换机是持久化交换机, RabbitMQ 重启后交换机仍然存在。
        auto_delete=false 表示交换机不会因为没有队列绑定、没有消费者或连接断开而自动删除。
    */
    channel->DeclareExchange(RabbitMqExchange,                     // exchange_name
                             amqp::Channel::EXCHANGE_TYPE_DIRECT,  // exchange_type
                             false,                                // passive
                             true,                                 // durable
                             false);                               // auto_delete

    /*
        passive=false 表示队列不存在时自动创建。
        durable=true 表示队列是持久化队列。
        exclusive=false 表示不是当前连接独占，后续可以启动多个消费者。
        auto_delete=false 表示没有消费者时队列也不会自动删除。
    */
    channel->DeclareQueue(RabbitMqQueue, // queue_name
                          false,         // passive
                          true,          // durable
                          false,         // exclusive
                          false);        // auto_delete

    // 绑定后，routing key 为 oss 的消息会进入 oss.queue。
    channel->BindQueue(RabbitMqQueue, RabbitMqExchange, RabbitMqRoutingKey);
}

RabbitMqOssUploader::RabbitMqOssUploader()
    : oss_storage_(nullptr)
    , stopping_(false)          // 设置停止标志为 false
{}

RabbitMqOssUploader::RabbitMqOssUploader(OssStorage& oss_storage)
    : oss_storage_(&oss_storage)
    , stopping_(false)          // 设置停止标志为 false
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

    stopping_ = false; // 启动前把停止标志重置为 false，表示允许 worker_loop 运行
    worker_ = thread(&RabbitMqOssUploader::worker_loop, this); // 创建后台线程，执行当前对象的 worker_loop 成员函数
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

bool RabbitMqOssUploader::save_temp_file(int uid,
                                         const string& hashcode,
                                         const string& content,
                                         string& temp_path)
{
    try {
        /*
            create_directories 会递归创建目录。
            如果 ./tmp 不存在，它会先创建 ./tmp，再创建 ./tmp/uploads。
            如果目录已经存在，也不会报错。
        */
        fs::create_directories(TempUploadDir);

        fs::path path = make_temp_file_path(uid, hashcode);

        /*
            ios::binary 表示按二进制方式写入。
            这样文本、图片、压缩包等任意文件内容都不会被换行转换影响。
        */
        ofstream ofs(path, ios::binary);
        if (!ofs) {
            cerr << "[TempFile write FAILED] open failed: " << path.string() << endl;
            return false;
        }

        ofs.write(content.data(), content.size());
        if (!ofs) {
            cerr << "[TempFile write FAILED] write failed: " << path.string() << endl;
            return false;
        }

        temp_path = path.string();
        cout << "[TempFile write OK] " << temp_path << endl;
        return true;
    } catch (const exception& ex) {
        cerr << "[TempFile write ERROR] " << ex.what() << endl;
        return false;
    }
}

void RabbitMqOssUploader::remove_temp_file(const string& temp_path)
{
    if (temp_path.empty()) {
        return;
    }

    /*
        使用 error_code 版本的 remove：
        - 删除失败时不会抛异常
        - 可以把错误打印出来，方便排查权限或路径问题
    */
    error_code ec;
    bool removed = fs::remove(temp_path, ec);
    if (ec) {
        cerr << "[TempFile remove FAILED] " << temp_path << ", error: " << ec.message() << endl;
        return;
    }

    if (removed) {
        cout << "[TempFile remove OK] " << temp_path << endl;
    }
}

bool RabbitMqOssUploader::publish(int uid, const string& hashcode, const string& temp_path)
{
    try {
        amqp::Channel::ptr_t channel = create_rabbitmq_channel();
        declare_rabbitmq_topology(channel);

        json task;
        /*
            uid/hashcode 决定 OSS ObjectName：
                users/{uid}/{hashcode}

            tempPath 指向 HTTP 上传接口保存的本地临时文件。
            RabbitMQ 消息只传路径，不再携带真实文件内容。
        */
        task["uid"] = uid;
        task["hashcode"] = hashcode;
        task["tempPath"] = temp_path;

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
        API Gateway 使用默认构造函数时，oss_storage_ 是 nullptr。
        网关只发布任务，不应该启动消费者线程。

        如果误调用了 start()，这里直接返回，避免解引用空指针。
    */
    if (oss_storage_ == nullptr) {
        cerr << "[RabbitMQ consumer ERROR] OssStorage is not set" << endl;
        return;
    }

    /*
        外层循环负责断线重连。
        如果 RabbitMQ 临时不可用，捕获异常后稍等几秒，再重新连接。
    */
    while (!stopping_) {
        try {
            amqp::Channel::ptr_t channel = create_rabbitmq_channel();
            declare_rabbitmq_topology(channel);

            /*
                使用 BasicConsume 推送模式订阅队列。

                参数含义：
                - no_ack=false：关闭自动确认，处理成功后手动 BasicAck
                - exclusive=false：不独占队列，后续可以启动多个消费者
                - message_prefetch_count=1：一次只推送一条未确认消息

                注意：这里要求 SimpleAmqpClient 的 basic.qos 使用 per-consumer QoS，
                即 qos.global=false。否则新版本 RabbitMQ 可能拒绝 global_qos。
            */
            string consumer_tag = channel->BasicConsume(RabbitMqQueue, // queue
                                                        "",            // consumer_tag
                                                        true,          // no_local
                                                        false,         // no_ack
                                                        false,         // exclusive
                                                        1);            // message_prefetch_count
            cout << "[RabbitMQ consumer] waiting for pushed messages from "
                 << RabbitMqQueue << endl;

            /*
                内层循环负责正常消费消息：
                1. BasicConsumeMessage 阻塞等待 RabbitMQ 推送消息
                2. timeout=1000ms，让线程能定期检查 stopping_ 并退出
                3. 收到消息后解析、上传、ack/reject
            */
            while (!stopping_) {
                amqp::Envelope::ptr_t envelope;
                if (!channel->BasicConsumeMessage(consumer_tag, envelope, 1000)) {
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
                    || !task.contains("tempPath")
                    || !task["tempPath"].is_string()) {
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
                string temp_path = task["tempPath"].get<string>();
                string content;

                /*
                    RabbitMQ 消息里只有临时文件路径。
                    消费者要先把这个临时文件读回内存，再调用 OSS SDK 上传。

                    如果临时文件已经不存在，说明这条任务无法继续完成。
                    重新入队也找不回文件，所以这里丢弃消息。
                */
                if (!read_temp_file(temp_path, content)) {
                    cerr << "[RabbitMQ consume FAILED] temp file missing or unreadable" << endl;
                    channel->BasicReject(envelope, false);
                    continue;
                }

                /*
                    真正耗时的 OSS PutObject 在后台线程中执行。
                    HTTP 上传接口只负责把任务投递到 RabbitMQ。
                */
                if (oss_storage_->upload_object(uid, hashcode, content)) {
                    channel->BasicAck(envelope);
                    /*
                        OSS 上传成功后，临时文件已经没有用了。
                        这里立即删除，避免本地磁盘越积越多。
                    */
                    remove_temp_file(temp_path);
                    cout << "[RabbitMQ consume OK] uid=" << uid << ", hashcode=" << hashcode << endl;
                } else {
                    /*
                        OSS 上传失败可能是临时网络或服务异常。
                        requeue=true 表示把消息放回队列，稍后继续重试。
                        注意：这里不能删除临时文件，因为下次重试还要读取它。
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
