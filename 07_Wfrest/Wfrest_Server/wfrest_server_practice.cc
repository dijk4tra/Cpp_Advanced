#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <signal.h>
#include <string>
#include <vector>

#include <workflow/MySQLResult.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>
#include <nlohmann/json.hpp>

#include "CryptoUtil.h"

using namespace std;
using namespace wfrest;
using namespace protocol;
using json = nlohmann::json;

static const string MYSQL_URL = "mysql://root:123456@localhost/demo";
static const string RESOURCE_ROOT = "resources";

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

/*
    从 x-www-form-urlencoded 表单里拿用户名和密码。

    curl 示例：
    curl -X POST http://127.0.0.1:8888/register \
         -H "Content-Type: application/x-www-form-urlencoded" \
         -d "username=alice&password=123456"
*/
bool get_username_password(const HttpReq *req, string& username, string& password)
{
    // if (req->content_type() != APPLICATION_URLENCODED) {
    //     return false;
    // }

    map<string, string>& form = req->form_kv();

    if (form.count("username")) {
        username = form["username"];
    } else {
        return false;
    }

    if (form.count("password")) {
        password = form["password"];
    } else {
        return false;
    }

    return !username.empty() && !password.empty();
}

// POST /register
void do_register(const HttpReq *req, HttpResp *resp)
{
    string username;
    string password;

    if (!get_username_password(req, username, password)) {
        resp->set_status(HttpStatusBadRequest);

        json result;
        result["code"] = 400;
        result["message"] = "Bad Request: 请使用 username/password 表单字段";

        resp->String(result.dump());
        return;
    }

    cout << "[register] username: " << username << endl;

    // 注册前查重：这个用户名现在是否已经存在？
    string check_sql =
        "SELECT id FROM tbl_user "
        "WHERE username='" + username + "' "
        "LIMIT 1;";

    cout << "[SQL] " << check_sql << endl;

    // 第一步：先查用户名是否已经存在。
    resp->MySQL(MYSQL_URL, check_sql, [resp, username, password](MySQLResultCursor *cursor) {
        if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
            resp->set_status(HttpStatusInternalServerError);

            json result;
            result["code"] = 500;
            result["message"] = "注册失败：数据库查询用户失败";

            resp->String(result.dump());
            return;
        }

        if (cursor->get_rows_count() != 0) { // 在数据库中已有该用户名
            resp->set_status(HttpStatusBadRequest);

            json result;
            result["code"] = 400;
            result["message"] = "用户名已存在";

            resp->String(result.dump());
            return;
        }

        // 第二步：用户名不存在，正式插入新用户。
        string salt = CryptoUtil::generate_salt();
        string hashcode = CryptoUtil::hash_password(password, salt);

        // 注册插入：需要判断 INSERT 是否执行成功
        string insert_sql =
            "INSERT INTO tbl_user (username, password, salt) VALUES ('" +
            username + "', '" +
            hashcode + "', '" +
            salt + "');";

        cout << "[SQL] " << insert_sql << endl;

        resp->MySQL(MYSQL_URL, insert_sql, [resp](MySQLResultCursor *cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_OK) { // INSERT 是否成功?
                resp->set_status(HttpStatusInternalServerError);

                json result;
                result["code"] = 500;
                result["message"] = "注册失败：数据库插入用户失败";

                resp->String(result.dump());
                return;
            }

            json result;
            result["code"] = 200;
            result["message"] = "恭喜您，注册成功！";

            resp->String(result.dump());
        });
    });
}

// POST /login
void do_login(const HttpReq *req, HttpResp *resp)
{
    string username;
    string password;

    if (!get_username_password(req, username, password)) {
        resp->set_status(HttpStatusBadRequest);
        resp->String("Bad Request: 请使用 username/password 表单字段\n");
        return;
    }

    cout << "[login] username: " << username << endl;

    // 登录查询：这个用户名是否唯一地查到了一个用户？
    string sql =
        "SELECT id, username, password, salt, created_at "
        "FROM tbl_user "
        "WHERE username='" + username + "' "
        "LIMIT 1;";

    cout << "[SQL] " << sql << endl;

    // 注意 password 要按值捕获!
    resp->MySQL(MYSQL_URL, sql, [resp, password](MySQLResultCursor *cursor) {
        if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT ||
            cursor->get_rows_count() != 1) { // 不是SELECT结果集或者没有唯一地查到一个用户
            resp->set_status(HttpStatusBadRequest);

            json result;
            result["code"] = 400;
            result["message"] = "用户名或密码错误";

            resp->String(result.dump());
            return;
        }

        vector<MySQLCell> record;

        if (cursor->fetch_row(record)) {
            User user;
            user.id = record[0].as_int();
            user.username = record[1].as_string();
            user.hashcode = record[2].as_string();
            user.salt = record[3].as_string();
            user.createdAt = record[4].as_string();

            string hashcode = CryptoUtil::hash_password(password, user.salt);

            if (hashcode == user.hashcode) {
                string token = CryptoUtil::generate_token(user);

                json result;
                result["code"] = 200;
                result["message"] = user.username + "，欢迎您";
                result["token"] = token;

                resp->String(result.dump());
                return;
            }
        }

        resp->set_status(HttpStatusBadRequest); // 400
        json result;
        result["code"] = 400;
        result["message"] = "用户名或密码错误";

        resp->String(result.dump());
    });
}

// 从 Authorization 请求头中解析 Bearer token。
//    请求头格式：
//    Authorization: Bearer xxxxxx
bool get_bearer_token(const HttpReq *req, string& token)
{
    if (!req->has_header("Authorization")) {
        return false;
    }

    const string& auth = req->header("Authorization");

    const string prefix = "Bearer ";
    if (auth.size() <= prefix.size()) {
        return false;
    }

    if (auth.substr(0, prefix.size()) != prefix) {
        return false;
    }

    token = auth.substr(prefix.size());
    return !token.empty();
}

// 防止路径穿越
bool is_safe_path(const string& path)
{
    if (path.empty()) {
        return false;
    }

    if (path.find("..") != string::npos) {
        return false;
    }

    return true;
}

// GET /xxx
void do_send(const HttpReq *req, HttpResp *resp)
{
    string token;
    if (!get_bearer_token(req, token)) {
        resp->set_status(HttpStatusUnauthorized);
        resp->String("<html>401 Unauthorized: missing token</html>");
        return;
    }

    User user;
    if (!CryptoUtil::verify_token(token, user)) {
        resp->set_status(HttpStatusUnauthorized);
        resp->String("<html>401 Unauthorized: invalid or expired token</html>");
        return;
    }

    cout << "[auth ok] user: " << user.username << endl;

    string path = req->current_path();

    if (path == "/") {
        path = "/index.html";
    }

    if (!is_safe_path(path)) {
        resp->set_status(HttpStatusBadRequest);
        resp->String("<html>Bad Request: unsafe path</html>");
        return;
    }

    string real_path = RESOURCE_ROOT + path;

    cout << "[file] " << real_path << endl;

    if (!filesystem::exists(real_path) || !filesystem::is_regular_file(real_path)) {
        resp->set_status(HttpStatusNotFound);
        resp->String("<html>404 Not Found</html>");
        return;
    }

    // 相当于: open + pread + append body 的过程
    resp->File(real_path);
}

int main()
{
    signal(SIGINT, sig_handler);

    srand(time(nullptr));

    HttpServer server;

    // 注册路由
    server.POST("/register", do_register); // POST /register
    server.POST("/login", do_login);       // POST /login
    server.GET("/*", do_send);             // GET  任意静态资源路径

    if (server.start(8888) == 0) {
        cout << "wfrest static resource server is running at http://127.0.0.1:8888" << endl;
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}
