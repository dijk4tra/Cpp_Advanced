#include "../common/ServiceCommon.h"
#include "../common/ServiceRegistry.h"
#include "../../rpc_gen/cloud_disk.srpc.h"

#include <csignal>
#include <iostream>
#include <vector>
#include <workflow/WFFacilities.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

/*
    UserService 用户资料查询微服务

    当前只有一个功能：
    - 根据 user_id 查询当前用户资料
*/
class UserServiceImpl : public cloud::disk::UserService::Service {
public:
    /*
        GetUserProfile 对应 HTTP 网关中的 GET /api/v1/user/me。

        网关先调用 AuthService.VerifyToken 得到 user_id，
        再调用这里查询数据库中的最新用户资料。
    */
    void GetUserProfile(cloud::disk::GetUserProfileRequest* request,
                        cloud::disk::GetUserProfileResponse* response,
                        srpc::RPCContext*) override
    {
        int user_id = request->user_id(); // 从 protobuf 请求中取出用户 id

        if (user_id <= 0) {
            // user_id <= 0 一定不是合法用户
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        // 根据用户 id 查询用户资料
        string sql =
            "SELECT id, username, created_at "
            "FROM tbl_user "
            "WHERE id=" + to_string(user_id) + " AND tomb=0 "
            "LIMIT 1;";

        bool found = false;    // found 表示是否查到了用户
        bool db_error = false; // db_error 表示 SQL 是否执行失败

        /*
            用普通变量保存从数据库中拷贝出来的字段。
            不保存 MySQLCell，是因为 cursor 生命周期只在回调中有效。
        */
        int db_user_id = 0;
        string username;
        string created_at;

        // 执行查询
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            // SELECT 成功时状态应该是 MYSQL_STATUS_GET_RESULT
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            vector<MySQLCell> row; // row 保存一行用户记录

            // 没有行表示这个 user_id 不存在，或用户已被逻辑删除
            if (!cursor.fetch_row(row)) {
                found = false;
                return;
            }

            // 到这里表示查询到了用户
            found = true;

            // 按 SELECT 字段顺序拷贝数据
            db_user_id = row[0].as_int();
            username = row[1].as_string();
            created_at = row[2].as_string();
        });

        // 查询任务失败或 SELECT 状态异常，都按服务端错误处理
        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        // 查不到用户时返回 404
        if (!found) {
            set_result(response->mutable_result(), 404, "用户不存在");
            return;
        }

        // 设置业务成功
        set_result(response->mutable_result(), 0, "获取个人信息成功");

        // 写入响应中的用户资料
        cloud::disk::UserIdentity* user = response->mutable_user();
        user->set_user_id(db_user_id);
        user->set_username(username);
        user->set_created_at(created_at);
    }
};

static WFFacilities::WaitGroup wait_group(1);

static void sig_handler(int)
{
    wait_group.done();
}

int main()
{
    // 初始化 protobuf
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // 注册 Ctrl+C 信号
    signal(SIGINT, sig_handler);

    unsigned short port = get_env_port("USER_SERVICE_PORT", 9002);

    /*
        读取注册到 Consul 时对外暴露的服务地址。

        本机运行时默认 127.0.0.1。
        如果 API Gateway 和 UserService 不在同一台机器上，
        这里必须配置成网关能访问到的地址。
    */
    string service_host = get_service_registry_host();

    // 创建 srpc server
    srpc::SRPCServer server;
    // 创建用户服务实现对象
    UserServiceImpl service;
    // 注册 UserService
    server.add_service(&service);

    // 启动服务
    if (server.start(port) == 0) {
        cout << "[UserService] listening on " << port << endl;

        /*
            UserService 启动成功后注册到 Consul。

            这样 API Gateway 后续可以按服务名 UserService 查询健康实例，
            而不是写死 127.0.0.1:9002。
        */
        ServiceRegistrar registrar("UserService", service_host, port);

        /*
            第五期要求服务必须注册到 Consul。
            如果注册失败，这里直接停止 srpc server 并退出。
        */
        if (!registrar.start()) {
            server.stop();
            google::protobuf::ShutdownProtobufLibrary();
            return 1;
        }

        wait_group.wait(); // 阻塞等待退出信号

        registrar.stop();  // 退出前停止 TTL 心跳，并从 Consul 注销当前服务实例

        server.stop();     // 停止 srpc server
    } else {
        cerr << "[UserService] start FAILED on port " << port << endl;
    }

    // 释放 protobuf 资源
    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}
