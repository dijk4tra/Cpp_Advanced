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

static string getEnvOrDefault(const char* name, const string& default_value)
{
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        return default_value;
    }
    return string(value);
}

static const string RabbitMqUri = getEnvOrDefault("RABBITMQ_URI", "amqp://guest:guest@localhost:5672/%2f");
static const string RabbitMqExchange = getEnvOrDefault("RABBITMQ_EXCHANGE", "oss.direct");
static const string RabbitMqQueue = getEnvOrDefault("RABBITMQ_QUEUE", "oss.queue");
static const string RabbitMqRoutingKey = getEnvOrDefault("RABBITMQ_ROUTING_KEY", "oss");

// RabbitMQ 消息只携带 tempPath，真实文件内容先落到本地临时目录。
static const string TempUploadDir = getEnvOrDefault("CLOUDDISK_TEMP_DIR", "./tmp/uploads");

static fs::path make_temp_file_path(int uid, const string& hashcode)
{
    auto now = chrono::system_clock::now().time_since_epoch();
    auto timestamp = chrono::duration_cast<chrono::nanoseconds>(now).count();
    string temp_filename = to_string(uid) + "-" + hashcode + "-" + to_string(timestamp) + ".tmp";
    return fs::path(TempUploadDir) / temp_filename;
}

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

static amqp::Channel::ptr_t create_rabbitmq_channel()
{
    return amqp::Channel::CreateFromUri(RabbitMqUri);
}

static void declare_rabbitmq_topology(const amqp::Channel::ptr_t& channel)
{
    channel->DeclareExchange(RabbitMqExchange,                     // exchange_name
                             amqp::Channel::EXCHANGE_TYPE_DIRECT,  // exchange_type
                             false,                                // passive
                             true,                                 // durable
                             false);                               // auto_delete

    channel->DeclareQueue(RabbitMqQueue, // queue_name
                          false,         // passive
                          true,          // durable
                          false,         // exclusive
                          false);        // auto_delete

    channel->BindQueue(RabbitMqQueue, RabbitMqExchange, RabbitMqRoutingKey);
}

RabbitMqOssUploader::RabbitMqOssUploader()
    : oss_storage_(nullptr)
    , stopping_(false)
{}

RabbitMqOssUploader::RabbitMqOssUploader(OssStorage& oss_storage)
    : oss_storage_(&oss_storage)
    , stopping_(false)
{}

RabbitMqOssUploader::~RabbitMqOssUploader()
{
    stop();
}

void RabbitMqOssUploader::start()
{
    if (worker_.joinable()) {
        return;
    }

    stopping_ = false;
    worker_ = thread(&RabbitMqOssUploader::worker_loop, this);
}

void RabbitMqOssUploader::stop()
{
    stopping_ = true;

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
        fs::create_directories(TempUploadDir);

        fs::path path = make_temp_file_path(uid, hashcode);

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
        task["uid"] = uid;
        task["hashcode"] = hashcode;
        task["tempPath"] = temp_path;

        amqp::BasicMessage::ptr_t message = amqp::BasicMessage::Create(task.dump());
        message->ContentType("application/json");
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
    if (oss_storage_ == nullptr) {
        cerr << "[RabbitMQ consumer ERROR] OssStorage is not set" << endl;
        return;
    }

    // 外层循环负责 RabbitMQ 断线重连。
    while (!stopping_) {
        try {
            amqp::Channel::ptr_t channel = create_rabbitmq_channel();
            declare_rabbitmq_topology(channel);

            // no_ack=false；上传成功后手动 ack，失败时按场景 reject/requeue。
            string consumer_tag = channel->BasicConsume(RabbitMqQueue, // queue
                                                        "",            // consumer_tag
                                                        true,          // no_local
                                                        false,         // no_ack
                                                        false,         // exclusive
                                                        1);            // message_prefetch_count
            cout << "[RabbitMQ consumer] waiting for pushed messages from "
                 << RabbitMqQueue << endl;

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
                    cerr << "[RabbitMQ consume FAILED] invalid message body" << endl;
                    channel->BasicReject(envelope, false);
                    continue;
                }

                int uid = task["uid"].get<int>();
                string hashcode = task["hashcode"].get<string>();
                string temp_path = task["tempPath"].get<string>();
                string content;

                if (!read_temp_file(temp_path, content)) {
                    cerr << "[RabbitMQ consume FAILED] temp file missing or unreadable" << endl;
                    channel->BasicReject(envelope, false);
                    continue;
                }

                if (oss_storage_->upload_object(uid, hashcode, content)) {
                    channel->BasicAck(envelope);
                    remove_temp_file(temp_path);
                    cout << "[RabbitMQ consume OK] uid=" << uid << ", hashcode=" << hashcode << endl;
                } else {
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
