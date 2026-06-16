#include "../common/CryptoUtil.h"
#include "../common/ServiceRegistry.h"
#include "../common/ServiceCommon.h"
#include "../../rpc_gen/cloud_disk.srpc.h"

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <vector>
#include <workflow/WFFacilities.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

//
/*
    AuthService 认证微服务

    它负责：
    - 注册用户
    - 登录用户
    - 生成 JWT
    - 校验 JWT
    - 访问 tbl_user

    服务端仍会校验来自网关的输入。
*/
class AuthServiceImpl : public cloud::disk::AuthService::Service {
public:
    /*
        Register 对应 HTTP 网关中的 POST /api/v1/auth/register。

        网关已经完成 JSON 解析和 confirm 检查。
        这里专心进行认证服务：写 tbl_user。
    */
    void Register(cloud::disk::RegisterRequest* request,
                cloud::disk::RegisterResponse* response,
                srpc::RPCContext*) override
    {
        /*
            先从 protobuf 请求中取出用户名和密码。
            protobuf 的 string 字段默认是空字符串，所以没有字段时也会得到 ""。
        */
        string username = request->username();
        string password = request->password();

        /*
            服务端仍然做一次基础校验。
            即使网关已经校验过，微服务也不应该完全相信调用方。
        */
        if (username.empty() || password.empty()) {
            set_result(response->mutable_result(), 400, "用户名和密码不能为空");
            return;
        }

        string salt = CryptoUtil::generate_salt(); // 生成随机 salt
        /*
            计算密码哈希。
            数据库中保存 password_hash，不保存明文 password。
        */
        string password_hash = CryptoUtil::hash_password(password, salt);

        /*
            拼接插入用户的 SQL。
            escape_sql() 处理用户名、哈希值、salt 中可能出现的特殊字符。
        */
        string sql =
            "INSERT INTO tbl_user (username, password, salt) VALUES ('" +
            escape_sql(username) + "', '" +
            escape_sql(password_hash) + "', '" +
            escape_sql(salt) + "');";

        /*
            insert_id 用来保存 MySQL 自动生成的用户 id。
            先初始化为 0，只有 INSERT 成功后才会被赋值。
        */
        int insert_id = 0;

        /*
            cursor_status 用来保存 MySQLResultCursor 的状态。
            INSERT 成功时应该是 MYSQL_STATUS_OK。
        */
        int cursor_status = MYSQL_STATUS_ERROR;

        /*
            执行 SQL。
            run_mysql_query 会等待 MySQL 任务完成，然后在 handler 中读取结果。
        */
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            // 记录 cursor 状态，方便回调外判断 INSERT 是否成功
            cursor_status = cursor.get_cursor_status();
            // get_insert_id() 返回 MySQL 为 AUTO_INCREMENT 字段生成的 id
            insert_id = static_cast<int>(cursor.get_insert_id());
        });

        /*
            query_ok=false 通常表示网络错误或 MySQL 返回错误包。
            在注册接口里最常见的业务原因是用户名唯一键冲突。
        */
        if (!query_ok || cursor_status != MYSQL_STATUS_OK) {
            set_result(response->mutable_result(), 409, "用户名已存在");
            return;
        }

        /*
            到这里说明注册成功。
            code=0 表示业务成功，HTTP 状态码由网关决定。
        */
        set_result(response->mutable_result(), 0, "注册成功");

        // 把新用户 id 和用户名写入响应，网关会转成前端需要的 JSON
        response->set_user_id(insert_id);
        response->set_username(username);
    }

    /*
        Login 对应 HTTP 网关中的 POST /api/v1/auth/login。

        它负责：
        1. 查询用户记录。
        2. 使用数据库中的 salt 重新计算密码哈希。
        3. 哈希一致则生成 JWT。
    */
    void Login(cloud::disk::LoginRequest* request,
            cloud::disk::LoginResponse* response,
            srpc::RPCContext*) override
    {
        // 从请求中取出用户名和密码
        string username = request->username();
        string password = request->password();

        if (username.empty() || password.empty()) {
            set_result(response->mutable_result(), 400, "用户名和密码不能为空");
            return;
        }

        string sql =
            "SELECT id, username, password, salt, created_at "
            "FROM tbl_user "
            "WHERE username='" + escape_sql(username) + "' AND tomb=0 "
            "LIMIT 1;";

        bool found = false;    // found 表示 SELECT 是否查到了用户行
        bool db_error = false; // db_error 表示 SELECT 本身是否失败
        User user;             // user 用来保存从数据库读出的用户记录

        // 执行 SELECT
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {

            // SELECT 成功时 cursor 状态应该是 MYSQL_STATUS_GET_RESULT
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            vector<MySQLCell> row; // row 保存当前读取到的一行数据

            // fetch_row(row) 返回 false 表示结果集没有任何行
            if (!cursor.fetch_row(row)) {
                found = false;
                return;
            }

            // 到这里说明查到了用户
            found = true;

            // 按 SELECT 字段顺序把 MySQLCell 转成 C++ 字段
            user.id         = row[0].as_int();
            user.username   = row[1].as_string();
            user.password   = row[2].as_string();
            user.salt       = row[3].as_string();
            user.createdAt  = row[4].as_string();
        });

        // 查询执行失败是服务端问题
        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        // 登录失败不区分用户不存在和密码错误，避免泄露账号存在性
        if (!found) {
            set_result(response->mutable_result(), 401, "用户名或密码错误");
            return;
        }

        // 使用数据库中的 salt 对用户输入的 password 再算一次哈希
        string input_hash = CryptoUtil::hash_password(password, user.salt);

        if (input_hash != user.password) { // 哈希不同，说明密码错误
            set_result(response->mutable_result(), 401, "用户名或密码错误");
            return;
        }

        // 密码正确，生成 JWT
        string token = CryptoUtil::generate_token(user);

        // 设置业务成功
        set_result(response->mutable_result(), 0, "登录成功");
        response->set_access_token(token);  // 写入 token 字符串
        response->set_token_type("Bearer"); // 当前项目使用 Bearer Token

        // mutable_user() 返回响应中 user 子对象的可写指针
        cloud::disk::UserIdentity* identity = response->mutable_user();

        // 把用户基础信息写入登录响应
        identity->set_user_id(user.id);
        identity->set_username(user.username);
        identity->set_created_at(user.createdAt);
    }

    /*
        VerifyToken 供 API Gateway 校验登录态使用。

        网关负责从 Authorization 头中解析 Bearer Token；
        AuthService 负责判断这个 token 是否有效。
    */
    void VerifyToken(cloud::disk::VerifyTokenRequest* request,
                    cloud::disk::VerifyTokenResponse* response,
                    srpc::RPCContext*) override
    {
        string token = request->access_token(); // 从请求中取出 token

        if (token.empty()) {
            set_result(response->mutable_result(), 401, "无效的访问令牌");
            return;
        }

        User user;
        // verify_token 会校验签名、主题、过期时间，并把用户信息写入 user
        if (!CryptoUtil::verify_token(token, user)) {
            set_result(response->mutable_result(), 401, "无效的访问令牌");
            return;
        }

        // token 有效, 设置业务成功
        set_result(response->mutable_result(), 0, "Token 校验成功");

        // 把 token 中的用户身份返回给网关
        auto* identity = response->mutable_user();
        identity->set_user_id(user.id);
        identity->set_username(user.username);
        identity->set_created_at(user.createdAt);
    }
};

/*
    服务进程退出等待器。
    SIGINT 到来时 done()，main() 中的 wait() 才会返回。
*/
static WFFacilities::WaitGroup wait_group(1);

// 按下 Ctrl+C 时触发这个函数
static void sig_handler(int)
{
    wait_group.done();
}

int main()
{
    // 初始化 protobuf 运行库
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // 注册 Ctrl+C 信号处理函数
    signal(SIGINT, sig_handler);

    /*
        设置 rand() 的随机种子。
        CryptoUtil::generate_salt() 内部使用 rand() 生成 salt，
        第三期是在 API Gateway 的 main.cc 中调用 srand()；
        第四期注册逻辑移到 AuthService 后，也要在 AuthService 进程中设置一次随机种子。
    */
    srand(time(nullptr));

    unsigned short port = get_env_port("AUTH_SERVICE_PORT", 9001);

    /*
        读取当前服务注册到 Consul 时使用的地址。

        本机环境默认是 127.0.0.1。
        如果把服务部署到其它机器，这里应该通过 CLOUDDISK_SERVICE_HOST
        配成 API Gateway 能访问到的内网 IP。
    */
    string service_host = get_service_registry_host();

    // 创建 srpc 服务器
    srpc::SRPCServer server;

    /*
        创建认证服务实现对象。
        这个对象必须比 server 运行时间更长，所以放在 main 栈上。
    */
    AuthServiceImpl service;

    // 把 AuthService 注册进 srpc server
    server.add_service(&service);

    // 启动监听
    if (server.start(port) == 0) {
        cout << "[AuthService] listening on " << port << endl;

        /*
            AuthService 启动成功后，向 Consul 注册当前服务实例。

            注意顺序：
            1. 先 server.start(port)，确保 srpc 端口已经监听；
            2. 再注册到 Consul，避免网关发现一个尚未真正可连接的实例。
        */
        ServiceRegistrar registrar("AuthService", service_host, port);

        /*
            start() 内部会注册服务并启动 TTL 心跳线程。

            第五期要求服务必须注册到 Consul。
            如果注册失败，这里直接停止 srpc server 并退出。
        */
        if (!registrar.start()) {
            server.stop();
            google::protobuf::ShutdownProtobufLibrary();
            return 1;
        }

        wait_group.wait(); // 阻塞等待 Ctrl+C

        registrar.stop();  // 退出前停止 TTL 心跳，并从 Consul 注销当前服务实例

        server.stop();     // 收到退出信号后停止服务
    } else {
        // start() 非 0 表示监听失败，例如端口被占用
        cerr << "[AuthService] start FAILED on port " << port << endl;
    }

    // 释放 protobuf 资源
    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}
