#include "../common/ServiceCommon.h"
#include "../../rpc_gen/cloud_disk.srpc.h"

#include <csignal>
#include <iostream>
#include <vector>
#include <workflow/WFFacilities.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

// UserService 提供用户资料查询能力。
class UserServiceImpl : public cloud::disk::UserService::Service {
public:
    void GetUserProfile(cloud::disk::GetUserProfileRequest* request,
                        cloud::disk::GetUserProfileResponse* response,
                        srpc::RPCContext*) override
    {
        int user_id = request->user_id();

        if (user_id <= 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        string sql =
            "SELECT id, username, created_at "
            "FROM tbl_user "
            "WHERE id=" + to_string(user_id) + " AND tomb=0 "
            "LIMIT 1;";

        bool found = false;
        bool db_error = false;

        int db_user_id = 0;
        string username;
        string created_at;

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

            db_user_id = row[0].as_int();
            username = row[1].as_string();
            created_at = row[2].as_string();
        });

        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        if (!found) {
            set_result(response->mutable_result(), 404, "用户不存在");
            return;
        }

        set_result(response->mutable_result(), 0, "获取个人信息成功");

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
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    signal(SIGINT, sig_handler);

    unsigned short port = get_env_port("USER_SERVICE_PORT", 9002);

    srpc::SRPCServer server;
    UserServiceImpl service;

    server.add_service(&service);

    if (server.start(port) == 0) {
        cout << "[UserService] listening on " << port << endl;

        wait_group.wait();
        server.stop();
    } else {
        cerr << "[UserService] start FAILED on port " << port << endl;
    }

    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}
