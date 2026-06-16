#include "ServiceRegistry.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <ppconsul/agent.h>
#include <ppconsul/consul.h>
#include <ppconsul/health.h>
#include <sstream>
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

    char* end = nullptr; // end 指向解析停止的位置
    long number = strtol(value, &end, 10); // strtol 用来把字符串转成长整数

    // 如果不是纯数字，或者小于等于 0，就认为配置无效。
    if (*end != '\0' || number <= 0) {
        return default_value;
    }

    // 当前配置值都很小，转 int 足够
    return static_cast<int>(number);
}

/*
    读取 Consul HTTP API 地址列表。

    三节点 Consul 使用逗号分隔的地址列表：
    CONSUL_HTTP_ADDRS=http://127.0.0.1:8500,http://127.0.0.1:8501,http://127.0.0.1:8502
*/
static vector<string> consul_http_addrs()
{
    // 读取三节点 Consul HTTP API 地址
    string raw_addrs = get_env_or_default(
        "CONSUL_HTTP_ADDRS",
        "http://127.0.0.1:8500,http://127.0.0.1:8501,http://127.0.0.1:8502");

    vector<string> addrs; // 保存最终可用的地址

    stringstream ss(raw_addrs); // 按逗号逐段读取字符串

    string item; // item 表示每次从逗号之间取出的一个地址

    // 持续读取，直到没有新的逗号分段。
    while (getline(ss, item, ',')) {
        addrs.push_back(item); // 地址之间只用英文逗号分隔，不在逗号前后添加空格
    }

    return addrs; // 返回地址列表
}

/*
    Consul 数据中心名称
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
    三节点改造后，HTTP 地址由调用方传入。
*/
static ppconsul::Consul create_consul_client(const string& consul_addr)
{
    return ppconsul::Consul(consul_addr, ppconsul::kw::dc = consul_dc());
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
    , active_consul_addr_() // 启动前还没有选中任何 Consul 节点
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
    // 读取 Consul HTTP API 地址列表。
    vector<string> addrs = consul_http_addrs();

    /*
        依次尝试每一个 Consul 地址。
        当前没有复杂负载均衡，谁先注册成功就先用谁。
    */
    for (const string& addr : addrs) {
        // register_to_consul 内部会捕获异常并打印失败原因。
        if (register_to_consul(addr)) {
            // 注册成功后，先标记状态，再启动心跳线程。
            {
                lock_guard<mutex> lock(mutex_);
                registered_ = true;
                stopping_ = false;
                active_consul_addr_ = addr;
            }

            // 启动后台心跳线程
            heartbeat_thread_ = thread(&ServiceRegistrar::heartbeat_loop, this);

            // 打印服务 ID 和 Consul 地址，方便在三节点环境排查当前注册到了哪个节点
            cout << "[Consul] registered " << service_id_
                 << " at " << host_ << ":" << port_
                 << " via " << addr << endl;

            return true;
        }
    }

    // 所有 Consul 地址都注册失败, 当前服务启动失败
    cerr << "[Consul] register FAILED for " << service_id_
         << ": no available Consul address" << endl;

    // 返回 false，让服务 main 函数停止启动
    return false;
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
        /*
            读取当前正在使用的 Consul 地址。
            正常情况下，它就是 start() 注册成功时保存的地址。
        */
        string consul_addr;
        {
            lock_guard<mutex> lock(mutex_);
            consul_addr = active_consul_addr_;
        }

        // 创建 Consul Agent 客户端
        ppconsul::Consul consul = create_consul_client(consul_addr);
        consul_agent::Agent agent(consul);

        // 正常退出时主动注销服务实例，避免 Consul UI 残留旧实例
        agent.deregisterService(service_id_);
        cout << "[Consul] deregistered " << service_id_
             << " via " << consul_addr << endl;
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
    // 心跳间隔来自环境变量，默认 5 秒
    const int heartbeat_seconds = consul_heartbeat_seconds();

    /*
        连续心跳失败次数。
        偶发一次失败可能只是网络抖动，所以这里连续失败 2 次后再切换 Consul。
    */
    int failed_count = 0;

    // 循环直到 stop() 把 stopping_ 改成 true
    while (true) {
        // 循环直到 stop() 把 stopping_ 改成 true
        string consul_addr;

        // 每轮先检查是否需要退出
        {
            lock_guard<mutex> lock(mutex_);
            if (stopping_) {
                break;
            }

            /*
                复制一份 active_consul_addr_。
                复制后本轮网络访问不再持有 mutex，避免阻塞 stop()。
            */
            consul_addr = active_consul_addr_;
        }

        try {
            /*
                心跳线程每一轮创建一个轻量 Consul/Agent 对象。
                这样切换 active_consul_addr_ 后，下一轮会自然使用新地址。
            */
            ppconsul::Consul consul = create_consul_client(consul_addr);
            consul_agent::Agent agent(consul);

            // 告诉 Consul：当前服务实例仍然健康
            agent.servicePass(service_id_);

            // 心跳成功后清零失败计数
            failed_count = 0;
        } catch (const exception& ex) {
            /*
                心跳失败通常是当前 Consul 节点不可用、网络不通，或者服务 ID
                没有注册在这个 Consul agent 上。
            */
            cerr << "[Consul] heartbeat FAILED for " << service_id_
                 << " via " << consul_addr
                 << ": " << ex.what() << endl;

            // 记录连续失败次数
            ++failed_count;

            // 连续失败 2 次后，尝试换一个 Consul 节点并重新注册
            if (failed_count >= 2) {
                /*
                    切换成功后清零失败次数。
                    切换失败也不退出业务服务，下一轮心跳会继续尝试。
                */
                if (reregister_to_available_consul(consul_addr)) {
                    failed_count = 0;
                }
            }
        }

        // 睡眠 heartbeat_seconds 秒后再发下一次心跳
        this_thread::sleep_for(chrono::seconds(heartbeat_seconds));
    }
}

bool ServiceRegistrar::register_to_consul(const string& consul_addr)
{
    try {
        // 创建指定地址的 Consul 客户端
        ppconsul::Consul consul = create_consul_client(consul_addr);

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

        // 注册成功
        return true;
    } catch (const exception& ex) {
        // 注册失败通常是 Consul 没启动、地址写错或网络不通
        cerr << "[Consul] register FAILED for " << service_id_
             << " via " << consul_addr
             << ": " << ex.what() << endl;

        // 返回 false，让调用方继续尝试下一个 Consul 地址
        return false;
    }
}

bool ServiceRegistrar::reregister_to_available_consul(const string& failed_consul_addr)
{
    // 读取 Consul 地址列表
    vector<string> addrs = consul_http_addrs();

    /*
        先尝试其它地址。
        如果 consul1 心跳失败，优先尝试 consul2/consul3。
    */
    for (const string& addr : addrs) {
        // 有多个地址时，先跳过刚刚失败的地址
        if (addrs.size() > 1 && addr == failed_consul_addr) {
            continue;
        }

        // 切换节点时必须重新注册当前服务实例
        if (register_to_consul(addr)) {
            // 注册成功后更新当前活跃 Consul 地址
            {
                lock_guard<mutex> lock(mutex_);
                active_consul_addr_ = addr;
                registered_ = true;
            }

            // 打印切换日志，方便观察故障转移
            cout << "[Consul] switched " << service_id_
                 << " from " << failed_consul_addr
                 << " to " << addr << endl;

            // 已经找到可用 Consul
            return true;
        }
    }

    /*
        如果只有一个地址，或者其它地址都失败，再给原地址一次机会。
        这样单节点配置仍然能按原逻辑持续重试。
    */
    if (register_to_consul(failed_consul_addr)) {
        // 原地址重新注册成功后，继续使用它
        {
            lock_guard<mutex> lock(mutex_);
            active_consul_addr_ = failed_consul_addr;
            registered_ = true;
        }

        // 打印恢复日志
        cout << "[Consul] re-registered " << service_id_
             << " via " << failed_consul_addr << endl;

        // 重注册成功
        return true;
    }

    /*
        所有地址都不可用。
        暂时不让业务服务直接退出，只打印日志并等待下一轮心跳继续尝试。
    */
    cerr << "[Consul] re-register FAILED for " << service_id_
         << ": no available Consul address" << endl;

    // 重注册失败
    return false;
}

bool ServiceDiscovery::select(const string& service_name,
                              ServiceEndpoint& endpoint)
{
    /*
        读取 Consul 地址列表。
        API Gateway 每次发现服务时按顺序尝试，某个 Consul HTTP API 不通就换下一个。
    */
    vector<string> addrs = consul_http_addrs();

    // 记录是否至少有一个 Consul API 正常响应
    bool any_consul_responded = false;

    // 依次尝试每一个 Consul 地址
    for (const string& addr : addrs) {
        try {
            // 创建当前地址的 Consul 客户端
            ppconsul::Consul consul = create_consul_client(addr);

            // Health API 可以按健康状态查询服务实例
            consul_health::Health health(consul);

            /*
                passing=true 表示只要健康实例。
                这比 catalog 查询更适合服务调用，因为 catalog 可能返回已经不健康的实例。
            */
            vector<consul_health::NodeServiceChecks> services =
                health.service(service_name, consul_health::kw::passing = true);

            // 能执行到这里，说明当前 Consul API 正常响应了
            any_consul_responded = true;

            // 保存可用 endpoint
            vector<ServiceEndpoint> endpoints;

            /*
                Consul 返回的是 tuple<Node, ServiceInfo, checks>。
                当前只需要 ServiceInfo 中的 address/port。
            */
            for (const auto& item : services) {
                // tuple 下标 1 的元素是服务实例信息
                const ppconsul::ServiceInfo& service = get<1>(item);

                // 注册服务时必须填写 address。如果 address 为空，直接跳过
                string host = service.address;

                // host 为空或 port 为 0，都不是可调用实例
                if (host.empty() || service.port == 0) {
                    continue;
                }

                // 保存一个健康实例地址
                endpoints.push_back(ServiceEndpoint { host, service.port });
            }

            // 如果 Consul 查到了健康实例，就做一次简单轮询
            if (!endpoints.empty()) {
                lock_guard<mutex> lock(mutex_);

                /*
                    取出当前服务上次轮询到的位置。
                    unordered_map 的 operator[] 在 key 不存在时会插入 0。
                */
                size_t& index = round_robin_index_[service_name];

                // 防止 index 比当前实例数量大。这种情况会发生在实例数量减少以后
                index = index % endpoints.size();

                // 选出本次使用的实例
                endpoint = endpoints[index];

                // 下次请求使用下一个实例
                index = (index + 1) % endpoints.size();

                // 打印当前使用的 Consul 地址，方便三节点故障切换时观察
                cout << "[Consul] discovered " << service_name
                     << " via " << addr << endl;

                return true;
            }
        } catch (const exception& ex) {
            /*
                查询失败通常是当前 Consul HTTP API 不可用或地址错误。
                这里不立即返回，而是继续尝试下一个 Consul 地址。
            */
            cerr << "[Consul] discovery FAILED for " << service_name
                 << " via " << addr
                 << ": " << ex.what() << endl;
        }
    }

    //  至少有一个 Consul 响应了，但所有响应里都没有 passing 实例
    if (any_consul_responded) {
        // Consul 正常响应但没有健康实例
        cerr << "[Consul] no passing instance for " << service_name << endl;
    } else {
        // 所有 Consul 地址都查询失败
        cerr << "[Consul] discovery FAILED for " << service_name
             << ": no available Consul address" << endl;
    }

    /*
        第五期不再退回固定地址。
        发现失败时直接返回 false，让 API Gateway 给前端返回错误响应。
    */
    return false;
}
