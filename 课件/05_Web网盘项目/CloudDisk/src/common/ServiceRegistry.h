#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

/*
    ServiceEndpoint 表示一个可以被 srpc 客户端连接的服务实例地址。

    注册中心最终解决的问题很简单：
    - 服务提供者告诉 Consul：“我这个服务实例在 host:port 上”；
    - 服务消费者从 Consul 查出：“我要调用的服务现在有哪些健康的 host:port”。

    所以这里先用一个最小结构保存 host 和 port。
*/
struct ServiceEndpoint {
    /*
        host 是服务实例对外暴露的 IP 或域名。

        本课程项目默认所有进程都在本机运行，所以通常是 127.0.0.1。
        如果以后服务跑到不同机器，就应该设置成其它机器可以访问到的内网 IP。
    */
    std::string host;

    /*
        port 是 srpc 服务监听端口。

        例如：
        - AuthService 默认 9001
        - UserService 默认 9002
        - FileMetaService 默认 9003
    */
    unsigned short port;
};

/*
    ServiceRegistrar 给“服务提供者”使用。

    当前会使用它的进程：
    - auth_service
    - user_service
    - filemeta_service

    它负责：
    1. 在服务启动成功后，把当前实例注册到 Consul；
    2. 后台定时发送 TTL 心跳，让 Consul 知道实例仍然健康；
    3. 服务退出时注销实例。
*/
class ServiceRegistrar {
public:
    /*
        service_name 是注册到 Consul 的服务名。

        这里建议和 Protobuf 服务名保持一致：
        - AuthService
        - UserService
        - FileMetaService
    */
    ServiceRegistrar(const std::string& service_name,
                     const std::string& host,
                     unsigned short port);

    /*
        析构函数里会调用 stop()。

        这样即使 main() 中忘记手动 stop，也会尽量停止心跳线程并注销服务。
    */
    ~ServiceRegistrar();

    /*
        启动注册流程。

        返回值表示注册是否成功：
        - true：注册成功，并且心跳线程已经启动；
        - false：注册失败，当前服务应该停止启动。
    */
    bool start();

    /*
        停止心跳并注销服务。

        正常 Ctrl+C 退出时，main() 会显式调用它。
    */
    void stop();

private:
    /*
        把当前服务实例注册到指定的 Consul HTTP API 地址。

        三节点 Consul 改造后，服务启动时会依次尝试多个 Consul 地址。
        这个函数把“向某个地址注册”的代码集中起来，避免 start()
        和心跳失败后的重注册流程重复写同一段 ppconsul 调用。
    */
    bool register_to_consul(const std::string& consul_addr);

    /*
        心跳连续失败后，尝试切换到其它 Consul 节点并重新注册当前实例。

        注意：
        TTL 心跳只能发送给已经知道当前 service_id 的 Consul agent。
        所以切换 Consul 节点时，不能只改心跳地址，必须先重新 registerService。
    */
    bool reregister_to_available_consul(const std::string& failed_consul_addr);

    /*
        心跳线程入口。

        Consul TTL 检查要求服务定期调用 servicePass(service_id)，
        否则超过 TTL 后实例会被标记成 critical。
    */
    void heartbeat_loop();

private:
    /*
        服务名，例如 AuthService。
    */
    std::string service_name_;

    /*
        实例 ID，例如 AuthService-127.0.0.1-9001。

        Consul 允许同一个服务名下有多个实例，但每个实例 ID 必须唯一。
    */
    std::string service_id_;

    /*
        当前实例对外暴露的地址。
    */
    std::string host_;

    /*
        当前实例监听的 srpc 端口。
    */
    unsigned short port_;

    /*
        当前已经成功注册到的 Consul HTTP API 地址。

        例如：
        - http://127.0.0.1:8500
        - http://127.0.0.1:8501
        - http://127.0.0.1:8502

        心跳线程会使用这个地址发送 servicePass。
    */
    std::string active_consul_addr_;

    /*
        表示是否已经成功注册到 Consul。

        只有注册成功后，stop() 才需要注销服务。
    */
    bool registered_;

    /*
        心跳线程是否需要退出。
    */
    bool stopping_;

    /*
        保护 stopping_ 和 registered_。

        心跳线程和主线程都会访问这两个变量，所以用一个简单互斥锁。
    */
    std::mutex mutex_;

    /*
        后台心跳线程。
    */
    std::thread heartbeat_thread_;
};

/*
    ServiceDiscovery 给“服务消费者”使用。

    当前会使用它的进程：
    - API Gateway，也就是 bin/server

    它负责：
    1. 按服务名从 Consul 查询 passing 状态实例；
    2. 在多个健康实例中做简单轮询；
*/
class ServiceDiscovery {
public:
    /*
        选择一个可用实例。

        参数：
        - service_name：要调用的服务名，例如 AuthService；
        - endpoint：输出参数，成功时写入最终选择的实例地址。

        返回值：
        - true：endpoint 可用；
        - false：Consul 查询失败，或没有任何健康实例。
    */
    bool select(const std::string& service_name,
                ServiceEndpoint& endpoint);

private:
    /*
        round_robin_index_ 保存每个服务下一次轮询使用的位置。

        例如 AuthService 有两个实例时：
        第一次选下标 0，第二次选下标 1，第三次又回到下标 0。
    */
    std::unordered_map<std::string, std::size_t> round_robin_index_;

    /*
        API Gateway 可能并发处理多个 HTTP 请求。

        多个请求同时选择实例时会同时修改 round_robin_index_，
        所以这里用 mutex 做最小保护。
    */
    std::mutex mutex_;
};

/*
    读取当前服务注册到 Consul 时使用的 host。

    单独暴露这个函数，是为了三个服务 main.cc 可以少写重复 getenv 代码。
*/
std::string get_service_registry_host();
