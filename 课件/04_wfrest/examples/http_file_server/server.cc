#include <iostream>
#include <signal.h>
#include <string>
#include <wfrest/HttpServer.h>
#include <wfrest/PathUtil.h>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>

#include "CryptoUtil.h"

using namespace std;
using namespace wfrest;
using namespace protocol;

static const char* DatabaseURL = "mysql://root:1234@localhost/demo";

void register_handler(const HttpReq* req, HttpResp* resp)
{
    // 1. 解析请求：获取用户名和密码
    if (req->content_type() != APPLICATION_URLENCODED) {
        resp->set_status(HttpStatusBadRequest);
        return;
    }
    map<string, string>& data = req->form_kv();
    string username = data["username"];
    string password = data["password"];

    // 2. 处理注册逻辑
    string salt = CryptoUtil::generate_salt();
    string hashcode = CryptoUtil::hash_password(password, salt);

    // 拼接SQL语句
    string sql = "INSERT INTO tbl_user (username, password, salt) VALUES ('" + username + "', '" + hashcode + "', '" + salt + "');";
    cout << "[SQL] " << sql << endl; /* 日志 */

    resp->MySQL(DatabaseURL, sql, [resp](MySQLResultCursor* cursor) {
        if (cursor->get_cursor_status() == MYSQL_STATUS_OK && cursor->get_affected_rows() == 1) {
            resp->String("恭喜您，注册成功！");
        } else {
            resp->set_status(HttpStatusBadRequest);
            resp->String("注册失败，用户名可能已存在！");
        }
    });
}

void login_handler(const HttpReq* req, HttpResp* resp)
{
    // 1. 解析请求：获取用户名和密码
    if (req->content_type() != APPLICATION_URLENCODED) {
        resp->set_status(HttpStatusBadRequest);
        return;
    }
    map<string, string>& data = req->form_kv();
    string username = data["username"];
    string password = data["password"];

    // 2. 处理登录逻辑
    string sql = "SELECT * from tbl_user WHERE username='" + username + "';";
    cout << "[SQL] " << sql << endl;

    resp->MySQL(DatabaseURL, sql, [resp, password](MySQLResultCursor* cursor) {
        if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT || cursor->get_rows_count() != 1) {
            resp->set_status(HttpStatusBadRequest);
            resp->String("登录失败，用户名或密码错误！");
            return;
        }

        vector<MySQLCell> record; /* 数据库中的一行又称为一条记录(record) */
        if (cursor->fetch_row(record)) {
            User user;
            user.id = record[0].as_int();
            user.username = record[1].as_string();
            user.password = record[2].as_string();
            user.salt = record[3].as_string();
            user.createdAt = record[4].as_datetime();

            string hashcode = CryptoUtil::hash_password(password, user.salt);
            if (hashcode == user.password) {
                resp->String(user.username + "，欢迎您!\n");
                resp->String(CryptoUtil::generate_token(user));
                return;
            }
        }
        /* 用户不存在: 空结果集或密码错误 */
        resp->set_status(HttpStatusBadRequest);
        resp->String("用户名或密码错误");
    });
}

void file_handler(const HttpReq* req, HttpResp* resp)
{
    // 1. 验证身份
    // Authorization: Bearer <jwt>
    string authHeader = req->header("Authorization");
    if (authHeader.empty() || authHeader.find("Bearer ") != 0) {
        resp->set_status(HttpStatusUnauthorized); // 401
        resp->String("身份验证失败\n");
        return;
    }
    // 校验token
    User user;
    string token = authHeader.substr(7);
    if (!CryptoUtil::verify_token(token, user)) {
        resp->set_status(HttpStatusUnauthorized); // 401
        resp->String("身份验证失败\n");
        return;
    }

    // 2. 处理文件请求
    string path = req->current_path();
    if (path == "/") {
        path += "index.html"; // 默认首页
    }
    path = "resources" + path;
    // 设置标头
    string filename = PathUtil::base(path);
    resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);
    resp->File(path);
}

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

int main()
{
    // 注册信号处理函数
    // 用户按下Ctrl-C终止程序
    signal(SIGINT, sig_handler);
    // 1. 创建HTTP服务器
    HttpServer server;

    // 2. 注册路由
    server.GET("/*", file_handler);
    server.POST("/login", login_handler);
    server.POST("/register", register_handler);

    // 3. 启动服务器
    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "ERROR: server start failed!" << endl;
        exit(1);
    }

    return 0;
}
