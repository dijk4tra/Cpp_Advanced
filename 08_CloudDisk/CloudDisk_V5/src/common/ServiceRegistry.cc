#include "ServiceRegistry.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <ppconsul/agent.h>
#include <ppconsul/consul.h>
#include <ppconsul/health.h>
#include <thread>
#include <tuple>
#include <vector>

using namespace std;

/*
    为 ppconsul 中常用类型起短别名。

    ppconsul::Consul 表示 Consul HTTP API 客户端。
    ppconsul::agent::Agent 用来注册服务和发送 TTL 心跳。
    ppconsul::health::Health 用来查询健康服务实例。
*/
namespace consul_agent = ppconsul::agent;
namespace consul_health = ppconsul::health;

/*
    从环境变量读取字符串。
    如果变量不存在或为空字符串，就返回默认值。
*/
static string get_env_or_default(const char* name, const string& default_value)
{
    /*
        getenv 返回 char*。
        如果环境变量没有设置，返回 nullptr。
    */
    const char* value = getenv(name);

    // 未设置或设置为空，都按“没有配置”处理
    if (value == nullptr || string(value).empty()) {
        return default_value;
    }

    // 转成 std::string，方便后续拼接和传参
    return string(value);
}

/*
    从环境变量读取整数。
    这里用于读取 TTL 秒数、心跳间隔秒数等简单配置。
*/
static int get_env_int(const char* name, int default_value)
{
    // 先拿到字符串形式的环境变量
    const char* value = getenv(name);

    // 没配置就返回默认值
    if (value == nullptr || string(value).empty()) {
        return default_value;
    }

    /*
        strtol 用来把字符串转成长整数。
        end 指向解析停止的位置。
    */
    char* end = nullptr;
    long number = strtol(value, &end, 10);

    /*
        如果不是纯数字，或者小于等于 0，就认为配置无效。
        本文件里的秒数配置都必须是正数。
    */
    if (*end != '\0' || number <= 0) {
        return default_value;
    }

    // 当前配置值都很小，转 int 足够
    return static_cast<int>(number);
}

/*
    Consul HTTP API 地址。
    Docker 本地启动 Consul 时，默认 UI 和 HTTP API 都暴露在 8500 端口。
*/
static string consul_http_addr()
{
    return get_env_or_default("CONSUL_HTTP_ADDR", "http://127.0.0.1:8500");
}

/*
    Consul 数据中心名称。
    PDF 示例里使用 dc1，这里也沿用 dc1 作为默认值。
*/
static string consul_dc()
{
    return get_env_or_default("CONSUL_DC", "dc1");
}

/*
    TTL 健康检查的超时时间。
    如果服务超过这个时间没有发送 servicePass，Consul 会把实例标记成 critical。
*/
static int consul_ttl_seconds()
{
    return get_env_int("CONSUL_TTL_SECONDS", 10);
}

/*
    心跳发送间隔。
    它必须小于 TTL，默认 5 秒配合 10 秒 TTL。
*/
static int consul_heartbeat_seconds()
{
    return get_env_int("CONSUL_HEARTBEAT_SECONDS", 5);
}

/*
    创建 Consul 客户端。
    ppconsul 构造 Consul 对象时需要传入 HTTP 地址和数据中心。
*/
static ppconsul::Consul create_consul_client()
{
    return ppconsul::Consul(consul_http_addr(), ppconsul::kw::dc = consul_dc());
}

/*
    生成服务实例 ID。

    Consul 中同一个 service_name 可以对应多个实例。
    每个实例用 service_id 区分，所以这里把服务名、host、port 拼起来。
*/
static string make_service_id(const string& service_name,
                              const string& host,
                              unsigned short port)
{
    return service_name + "-" + host + "-" + to_string(port);
}

string get_service_registry_host()
{
    /*
        CLOUDDISK_SERVICE_HOST 表示服务注册到 Consul 时暴露给消费者的地址。

        本地环境部署默认 127.0.0.1。
        多机部署时不能继续用 127.0.0.1，而应该改成其它机器能访问到的内网 IP。
    */
    return get_env_or_default("CLOUDDISK_SERVICE_HOST", "127.0.0.1");
}

ServiceRegistrar::ServiceRegistrar(const string& service_name,
                                   const string& host,
                                   unsigned short port)
    : service_name_(service_name) // 保存服务名
    , service_id_(make_service_id(service_name, host, port)) // 根据服务名、host、port 生成唯一实例 ID
    , host_(host) // 保存对外暴露 host
    , port_(port) // 保存监听端口
    , registered_(false) // 构造时还没有注册成功
    , stopping_(false) // 构造时心跳线程不需要退出
{}

ServiceRegistrar::~ServiceRegistrar()
{
    /*
        析构时调用 stop()。
        如果 main() 已经显式 stop()，这里再次调用也安全。
    */
    stop();
}

bool ServiceRegistrar::start()
{
    try {
        // 创建 Consul 客户端
        ppconsul::Consul consul = create_consul_client();

        // Agent API 负责注册当前进程提供的服务
        consul_agent::Agent agent(consul);

        /*
            把当前 srpc 服务注册到 Consul。
            kw::check 使用 TTL 健康检查，后续由 heartbeat_loop 定时 servicePass。
        */
        agent.registerService(
            consul_agent::kw::id = service_id_,
            consul_agent::kw::name = service_name_,
            consul_agent::kw::address = host_,
            consul_agent::kw::port = port_,
            consul_agent::kw::tags = ppconsul::Tags { "srpc", "cloud-disk" },
            consul_agent::kw::check = consul_agent::TtlCheck { chrono::seconds(consul_ttl_seconds()) });

        // 注册成功后，先标记状态，再启动心跳线程
        {
            lock_guard<mutex> lock(mutex_);
            registered_ = true;
            stopping_ = false;
        }

        // 启动后台心跳线程
        heartbeat_thread_ = thread(&ServiceRegistrar::heartbeat_loop, this);

        // 打印服务 ID，方便在 Consul UI 和日志之间对应
        cout << "[Consul] registered " << service_id_
             << " at " << host_ << ":" << port_ << endl;

        return true;
    } catch (const exception& ex) {
        // 注册失败通常是 Consul 没启动、地址写错或网络不通
        cerr << "[Consul] register FAILED for " << service_id_
             << ": " << ex.what() << endl;

        /*
            第五期要求必须通过 Consul 注册中心完成服务注册。
            注册失败时，当前服务应该启动失败，避免网关发现不到实例。
        */
        return false;
    }
}

void ServiceRegistrar::stop()
{
    // 先让心跳线程退出
    {
        lock_guard<mutex> lock(mutex_);
        stopping_ = true;
    }

    /*
        等心跳线程真正结束。
        joinable() 用来判断线程是否已经启动过且还没 join。
    */
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    // 只有成功注册过，才需要注销
    bool need_deregister = false;
    {
        lock_guard<mutex> lock(mutex_);
        need_deregister = registered_;
        registered_ = false;
    }

    // 没注册成功时直接返回
    if (!need_deregister) {
        return;
    }

    try {
        // 创建 Consul Agent 客户端
        ppconsul::Consul consul = create_consul_client();
        consul_agent::Agent agent(consul);

        // 正常退出时主动注销服务实例，避免 Consul UI 残留旧实例
        agent.deregisterService(service_id_);
        cout << "[Consul] deregistered " << service_id_ << endl;
    } catch (const exception& ex) {
        /*
            注销失败不影响进程退出，只打印日志。
            即使没注销成功，TTL 超时后 Consul 也会把实例标记为 critical。
        */
        cerr << "[Consul] deregister FAILED for " << service_id_
             << ": " << ex.what() << endl;
    }
}

void ServiceRegistrar::heartbeat_loop()
{
    /*
        心跳线程单独创建 Consul 客户端。
        这样 start() 函数里的局部 Consul/Agent 对象销毁后，心跳仍然可以继续工作。
    */
    ppconsul::Consul consul = create_consul_client();
    consul_agent::Agent agent(consul);

    // 心跳间隔来自环境变量，默认 5 秒
    const int heartbeat_seconds = consul_heartbeat_seconds();

    // 循环直到 stop() 把 stopping_ 改成 true
    while (true) {
        // 每轮先检查是否需要退出
        {
            lock_guard<mutex> lock(mutex_);
            if (stopping_) {
                break;
            }
        }

        try {
            // 告诉 Consul：当前服务实例仍然健康
            agent.servicePass(service_id_);
        } catch (const exception& ex) {
            /*
                心跳失败通常是 Consul 临时不可用。
                学习项目里先只打印日志，不做复杂重连状态机。
            */
            cerr << "[Consul] heartbeat FAILED for " << service_id_
                 << ": " << ex.what() << endl;
        }

        // 睡眠 heartbeat_seconds 秒后再发下一次心跳
        this_thread::sleep_for(chrono::seconds(heartbeat_seconds));
    }
}

bool ServiceDiscovery::select(const string& service_name,
                              ServiceEndpoint& endpoint)
{
    try {
        // 创建 Consul 客户端
        ppconsul::Consul consul = create_consul_client();

        // Health API 可以按健康状态查询服务实例
        consul_health::Health health(consul);

        /*
            passing=true 表示只要健康实例。
            这比 catalog 查询更适合服务调用，因为 catalog 可能返回已经不健康的实例。
        */
        vector<consul_health::NodeServiceChecks> services =
            health.service(service_name, consul_health::kw::passing = true);

        // 保存可用 endpoint
        vector<ServiceEndpoint> endpoints;

        /*
            Consul 返回的是 tuple<Node, ServiceInfo, checks>。
            当前只需要 ServiceInfo 中的 address/port。
        */
        for (const auto& item : services) {
            // tuple 第 1 个元素是服务实例信
            const ppconsul::ServiceInfo& service = get<1>(item);

            /*
                本项目注册服务时必须填写 address。
                如果 address 为空，就说明注册信息不符合第五期约定，直接跳过。
            */
            string host = service.address;

            // host 为空或 port 为 0，都不是可调用实例
            if (host.empty() || service.port == 0) {
                continue;
            }

            // 保存一个健康实例地址
            endpoints.push_back(ServiceEndpoint { host, service.port });
        }

        /*
            如果 Consul 查到了健康实例，就做一次简单轮询。
        */
        if (!endpoints.empty()) {
            lock_guard<mutex> lock(mutex_);

            /*
                取出当前服务上次轮询到的位置。
                unordered_map 的 operator[] 在 key 不存在时会插入 0。
            */
            size_t& index = round_robin_index_[service_name];

            /*
                防止 index 比当前实例数量大。
                这种情况会发生在实例数量减少以后。
            */
            index = index % endpoints.size();

            endpoint = endpoints[index]; // 选出本次使用的实例

            index = (index + 1) % endpoints.size(); // 下次请求使用下一个实例

            return true;
        }

        // Consul 正常响应但没有健康实例
        cerr << "[Consul] no passing instance for " << service_name << endl;
    } catch (const exception& ex) {

        // 查询失败通常是 Consul 不可用或地址错误
        cerr << "[Consul] discovery FAILED for " << service_name
             << ": " << ex.what() << endl;
    }

    /*
        第五期不再退回固定地址。
        发现失败时直接返回 false，让 API Gateway 给前端返回错误响应。
    */
    return false;
}
