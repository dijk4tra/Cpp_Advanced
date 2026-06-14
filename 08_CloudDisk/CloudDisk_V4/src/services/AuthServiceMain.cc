#include "../common/CryptoUtil.h"
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

// AuthService 负责注册、登录和 JWT 校验；服务端仍会校验来自网关的输入。
class AuthServiceImpl : public cloud::disk::AuthService::Service {
public:

void Register(cloud::disk::RegisterRequest* request,
              cloud::disk::RegisterResponse* response,
              srpc::RPCContext*) override
{
    string username = request->username();
    string password = request->password();

    if (username.empty() || password.empty()) {
        set_result(response->mutable_result(), 400, "用户名和密码不能为空");
        return;
    }

    string salt = CryptoUtil::generate_salt();
    string password_hash = CryptoUtil::hash_password(password, salt);

    string sql =
        "INSERT INTO tbl_user (username, password, salt) VALUES ('" +
        escape_sql(username) + "', '" +
        escape_sql(password_hash) + "', '" +
        escape_sql(salt) + "');";

    int insert_id = 0;
    int cursor_status = MYSQL_STATUS_ERROR;

    bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
        cursor_status = cursor.get_cursor_status();
        insert_id = static_cast<int>(cursor.get_insert_id());
    });

    if (!query_ok || cursor_status != MYSQL_STATUS_OK) {
        set_result(response->mutable_result(), 409, "用户名已存在");
        return;
    }

    set_result(response->mutable_result(), 0, "注册成功");
    response->set_user_id(insert_id);
    response->set_username(username);
}

void Login(cloud::disk::LoginRequest* request,
           cloud::disk::LoginResponse* response,
           srpc::RPCContext*) override
{
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

    bool found = false;
    bool db_error = false;
    User user;

    bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {

        if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
            db_error = true;
            return;
        }

        vector<MySQLCell> row;

        if (!cursor.fetch_row(row)) {
            found = false;
            return;
        }

        found = true;

        user.id         = row[0].as_int();
        user.username   = row[1].as_string();
        user.password   = row[2].as_string();
        user.salt       = row[3].as_string();
        user.createdAt  = row[4].as_string();
    });

    if (!query_ok || db_error) {
        set_result(response->mutable_result(), 500, "内部服务器错误");
        return;
    }

    // 登录失败不区分用户不存在和密码错误，避免泄露账号存在性。
    if (!found) {
        set_result(response->mutable_result(), 401, "用户名或密码错误");
        return;
    }

    string input_hash = CryptoUtil::hash_password(password, user.salt);

    if (input_hash != user.password) {
        set_result(response->mutable_result(), 401, "用户名或密码错误");
        return;
    }

    string token = CryptoUtil::generate_token(user);

    set_result(response->mutable_result(), 0, "登录成功");
    response->set_access_token(token);
    response->set_token_type("Bearer");

    auto* identity = response->mutable_user();
    identity->set_user_id(user.id);
    identity->set_username(user.username);
    identity->set_created_at(user.createdAt);
}

void VerifyToken(cloud::disk::VerifyTokenRequest* request,
                 cloud::disk::VerifyTokenResponse* response,
                 srpc::RPCContext*) override
{
    string token = request->access_token();

    if (token.empty()) {
        set_result(response->mutable_result(), 401, "无效的访问令牌");
        return;
    }

    User user;

    if (!CryptoUtil::verify_token(token, user)) {
        set_result(response->mutable_result(), 401, "无效的访问令牌");
        return;
    }

    set_result(response->mutable_result(), 0, "Token 校验成功");

    auto* identity = response->mutable_user();
    identity->set_user_id(user.id);
    identity->set_username(user.username);
    identity->set_created_at(user.createdAt);
}
};

static WFFacilities::WaitGroup wait_group(1);

static void sig_handler(int)
{
    wait_group.done();
}

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    signal(SIGINT, sig_handler);

    srand(time(nullptr));

    unsigned short port = get_env_port("AUTH_SERVICE_PORT", 9001);

    srpc::SRPCServer server;
    AuthServiceImpl service;

    server.add_service(&service);

    if (server.start(port) == 0) {
        cout << "[AuthService] listening on " << port << endl;

        wait_group.wait();
        server.stop();
    } else {
        cerr << "[AuthService] start FAILED on port " << port << endl;
    }

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
