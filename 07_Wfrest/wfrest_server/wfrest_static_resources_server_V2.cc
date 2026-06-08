#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <signal.h>
#include <string>
#include <vector>

#include <workflow/MySQLResult.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>
#include <wfrest/HttpServer.h>
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
    简单处理 SQL 字符串中的单引号。

    例如：
    username = abc'def

    如果不处理，拼接 SQL 会变成：
    'abc'def'

    SQL 字符串会被提前截断，甚至有 SQL 注入风险。

    注意：正式项目应该用参数化查询。
    但 workflow / wfrest 课堂作业里通常先用字符串拼接。
*/
string escape_sql(const string& s)
{
    string res;
    for (char ch : s) {
        if (ch == '\'') {
            res += "\\'";
        } else if (ch == '\\') {
            res += "\\\\";
        } else {
            res += ch;
        }
    }
    return res;
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

void set_json_response(HttpResp *resp, int status, int code, const string& message)
{
    resp->set_status(status);

    json result;
    result["code"] = code;
    result["message"] = message;

    resp->String(result.dump());
}

bool handle_mysql_error(WFMySQLTask *task, HttpResp *resp, const string& action, bool username_conflict)
{
    if (task->get_state() != WFT_STATE_SUCCESS) {
        string message = action + " failed: " +
            WFGlobal::get_error_string(task->get_state(), task->get_error());
        set_json_response(resp, HttpStatusInternalServerError, 500, message);
        return true;
    }

    MySQLResponse *mysql_resp = task->get_resp();
    if (mysql_resp->get_packet_type() == MYSQL_PACKET_ERROR) {
        if (username_conflict && mysql_resp->get_error_code() == 1062) {
            set_json_response(resp, HttpStatusConflict, 409, "用户名已存在");
            return true;
        }

        string message = action + " failed: mysql error " +
            to_string(mysql_resp->get_error_code()) + ": " +
            mysql_resp->get_error_msg();
        set_json_response(resp, HttpStatusInternalServerError, 500, message);
        return true;
    }

    return false;
}

/*
    POST /register

    流程：
    1. 解析 username/password
    2. 生成 salt
    3. hash_password(password, salt)
    4. INSERT INTO tbl_user
    5. 根据 MySQL 执行结果返回响应
*/
void do_register(const HttpReq *req, HttpResp *resp, SeriesWork *series)
{
    string username;
    string password;

    if (!get_username_password(req, username, password)) {
        set_json_response(resp, HttpStatusBadRequest, 400,
                          "Bad Request: 请使用 username/password 表单字段");
        return;
    }

    cout << "[register] username: " << username << endl;

    string check_sql =
        "SELECT id FROM tbl_user "
        "WHERE username='" + escape_sql(username) + "' "
        "LIMIT 1;";

    cout << "[SQL] " << check_sql << endl;

    /*
        第一步：先查用户名是否已经存在。

        注意：
        username 和 password 都要按值捕获。
        因为 MySQL 是异步任务，do_register() 返回后，
        局部变量 username/password 会销毁。
    */
    WFMySQLTask *check_task = WFTaskFactory::create_mysql_task(
        MYSQL_URL,
        3,
        [resp, username, password](WFMySQLTask *task) {
        if (handle_mysql_error(task, resp, "register check user", false)) {
            return;
        }

        MySQLResponse *mysql_resp = task->get_resp();
        MySQLResultCursor cursor(mysql_resp);
        vector<MySQLCell> record;

        if (cursor.fetch_row(record)) {
            set_json_response(resp, HttpStatusConflict, 409, "用户名已存在");
            return;
        }

        // 第二步：用户名不存在，正式插入新用户。
        string salt = CryptoUtil::generate_salt();
        string hashcode = CryptoUtil::hash_password(password, salt);

        string insert_sql =
            "INSERT INTO tbl_user (username, password, salt) VALUES ('" +
            escape_sql(username) + "', '" +
            escape_sql(hashcode) + "', '" +
            escape_sql(salt) + "');";

        cout << "[SQL] " << insert_sql << endl;

        WFMySQLTask *insert_task = WFTaskFactory::create_mysql_task(
            MYSQL_URL,
            3,
            [resp](WFMySQLTask *task) {
            if (handle_mysql_error(task, resp, "register insert user", true)) {
                return;
            }

            json result;
            result["code"] = 200;
            result["message"] = "恭喜您，注册成功！";

            resp->String(result.dump());
        });

        insert_task->get_req()->set_query(insert_sql);
        series_of(task)->push_back(insert_task);
    });

    check_task->get_req()->set_query(check_sql);
    series->push_back(check_task);
}

/*
    POST /login

    流程：
    1. 解析 username/password
    2. SELECT 用户信息
    3. 取出数据库中的 salt 和 password
    4. hash_password(用户输入的 password, 数据库 salt)
    5. 比对 hash
    6. 成功则生成 token
*/
void do_login(const HttpReq *req, HttpResp *resp, SeriesWork *series)
{
    string username;
    string password;

    if (!get_username_password(req, username, password)) {
        resp->set_status(HttpStatusBadRequest);
        resp->String("Bad Request: 请使用 username/password 表单字段\n");
        return;
    }

    cout << "[login] username: " << username << endl;

    string sql =
        "SELECT id, username, password, salt, created_at "
        "FROM tbl_user "
        "WHERE username='" + escape_sql(username) + "' "
        "LIMIT 1;";

    cout << "[SQL] " << sql << endl;

    /*
        注意这里要把 password 按值捕获进去。

        为什么？
        do_login 函数返回后，局部变量 password 就销毁了。
        MySQL 是异步任务，callback 未来才执行。
        所以不能用引用捕获 [&password]。
    */
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(
        MYSQL_URL,
        3,
        [resp, password](WFMySQLTask *task) {
        if (handle_mysql_error(task, resp, "login query user", false)) {
            return;
        }

        MySQLResponse *mysql_resp = task->get_resp();
        MySQLResultCursor cursor(mysql_resp);
        vector<MySQLCell> record;

        if (cursor.fetch_row(record)) {
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

    mysql_task->get_req()->set_query(sql);
    series->push_back(mysql_task);
}

/*
    从 Authorization 请求头中解析 Bearer token。

    请求头格式：
    Authorization: Bearer xxxxxx

    返回：
    - 成功：true，并把 token 写入 token 参数
    - 失败：false
*/
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

/*
    防止路径穿越。

    例如用户请求：
    GET /../../etc/passwd

    如果直接拼接：
    resources/../../etc/passwd

    就可能读到 resources 外面的文件。

    这里先做一个简单防护：
    1. 拒绝包含 ".." 的路径
    2. 拒绝空路径
*/
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

/*
    GET /xxx

    要求：
    1. 先校验 Token
    2. Token 不存在 / 无效 / 超时，返回 401
    3. Token 正确后，返回 resources 目录下的文件

    wfrest 里可以用 req->current_path() 获取当前请求路径。
    wfrest 文档中也说明了 current_path 表示当前实际请求路径。
*/
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

    /*
        wfrest 推荐的文件返回方式是 resp->File(path)。
        官方 Send File 文档也是这样写的：
            resp->File("todo.txt");
            resp->File("html/index.html");
        这相当于把你原来手动 open + pread + append body 的过程封装起来了。
    */
    resp->File(real_path);
}

int main()
{
    signal(SIGINT, sig_handler);

    srand(time(nullptr));

    HttpServer svr;

    /*
        注册路由。

        这三个路由分别对应：
        POST /register
        POST /login
        GET  任意静态资源路径
    */
    svr.POST("/register", do_register);
    svr.POST("/login", do_login);

    // wfrest 支持通配路由。
    // 这里用 "/*" 接住所有 GET 请求。
    // 也可以写成 "/{path}*" 这类形式，具体取决于你本机 wfrest 版本。
    svr.GET("/*", do_send);

    if (svr.start(8888) == 0) {
        cout << "wfrest static resource server is running at http://127.0.0.1:8888" << endl;
        waitGroup.wait();
        svr.stop();
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}
