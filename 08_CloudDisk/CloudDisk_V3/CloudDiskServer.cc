#include "CloudDiskServer.h"
#include "CryptoUtil.h"
#include "OssStorage.h"
#include "common.h"
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>
#include <wfrest/HttpDef.h>
#include <wfrest/HttpFile.h>
#include <wfrest/HttpMsg.h>
#include <wfrest/PathUtil.h>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/MySQLResult.h>
#include <workflow/Workflow.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace std::placeholders;
using namespace wfrest;
using namespace protocol;
using json = nlohmann::json;

// 数据库的URL
static const string DatabaseURL = "mysql://root:123456@localhost/CloudDisk";
// 最大重试次数
static const int RetryMax = 3;

static void response_json(HttpResp* resp, int status_code, const json& body)
{
    resp->set_status(status_code);
    resp->add_header("Content-Type", "application/json");
    resp->String(body.dump());
}

static void response_success(HttpResp* resp, int status_code, const string& message, const json& data)
{
    json body;
    body["status"] = "success";
    body["message"] = message;
    body["data"] = data;
    response_json(resp, status_code, body);
}

static void response_error(HttpResp* resp, int status_code, const string& message)
{
    json body;
    body["status"] = "error";
    body["message"] = message;
    response_json(resp, status_code, body);
}

static string escape_sql(const string& s)
{
    // 简单处理 SQL 字符串中的单引号和反斜线
    string result;
    for (char ch : s) {
        if (ch == '\'') {
            result += "\\'";
        } else if (ch == '\\') {
            result += "\\\\";
        } else {
            result += ch;
        }
    }
    return result;
}

// 生成 Content-Disposition 中 filename= 使用的 ASCII 文件名
static string content_disposition_fallback_filename(const string& filename)
{
    string result;
    for (unsigned char ch : filename) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += ch;
        } else if (ch >= 0x20 && ch <= 0x7e) {
            result += ch;
        } else {
            result += '_';
        }
    }

    if (result.empty()) {
        return "download";
    }
    return result;
}

// 判断一个字符是否可以直接出现在 RFC 5987 的 filename* 参数中
static bool is_rfc5987_attr_char(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') {
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return true;
    }
    if (ch >= 'a' && ch <= 'z') {
        return true;
    }

    switch (ch) {
    case '!':
    case '#':
    case '$':
    case '&':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

// 把 UTF-8 文件名编码成 filename* 可用的格式
static string encode_rfc5987_filename(const string& filename)
{
    static const char* hex = "0123456789ABCDEF";
    string result = "UTF-8''";

    for (unsigned char ch : filename) {
        if (is_rfc5987_attr_char(ch)) {
            result += ch;
        } else {
            result += '%';
            result += hex[ch >> 4];
            result += hex[ch & 0x0f];
        }
    }

    return result;
}

// 解析 JSON 请求体
static bool parse_json_body(const HttpReq* req, json& body)
{
    // 注册和登录都要求 Content-Type: application/json
    if (req->content_type() != APPLICATION_JSON) {
        return false;
    }

    body = json::parse(req->body(), nullptr, false);
    // nlohmann::json::parse(..., false) 在解析失败时不会抛异常，而是返回 discarded 状态
    return !body.is_discarded();
}

// 从 JSON 中读取字符串字段
static string json_string(const json& body, const string& key)
{
    if (!body.contains(key) || !body[key].is_string()) {
        return "";
    }
    return body[key].get<string>();
}

// 从 Authorization 头中取 Bearer Token
static bool get_bearer_token(const HttpReq* req, string& token)
{
    if (!req->has_header("Authorization")) {
        return false;
    }

    const string& authorization = req->header("Authorization");
    const string prefix = "Bearer ";
    if (authorization.size() <= prefix.size()) {
        return false;
    }

    if (authorization.substr(0, prefix.size()) != prefix) {
        return false;
    }

    token = authorization.substr(prefix.size());
    return !token.empty();
}

// 校验登录态
static bool check_login(const HttpReq* req, User& user)
{
    string token;
    if (!get_bearer_token(req, token)) {
        return false;
    }

    return CryptoUtil::verify_token(token, user);
}

CloudDiskServer::CloudDiskServer()
    : oss_storage_()
    , oss_uploader_(oss_storage_)
{
    /*
        CloudDiskServer 负责组织各个模块：
        - oss_storage_ 负责 OSS SDK 生命周期和对象上传/下载
        - oss_uploader_ 负责 RabbitMQ 任务发布和后台消费
        - 这里启动后台消费者，让异步备份能力随服务器一起启动
    */
    oss_uploader_.start();
}

CloudDiskServer::~CloudDiskServer()
{
    /*
        先停止 RabbitMQ 后台消费者。
        这样 OssStorage 析构并释放 OSS SDK 前，不会再有后台线程上传文件。
    */
    oss_uploader_.stop();
}

void CloudDiskServer::register_routes()
{
    // 设置静态资源的路由
    register_www_module();
    register_auth_module();
    register_user_module();
    register_file_module();
}

void CloudDiskServer::register_www_module()
{
    server_.Static("/", "./www/index.html");
    server_.Static("/static", "./www/static");
}

void CloudDiskServer::register_auth_module() // 注册/登陆模块
{
    /*
        POST /api/v1/auth/register

        注册流程：
        1. 检查请求类型必须是 JSON
        2. 读取 username/password/confirm
        3. 检查用户名/密码是否为空以及两次密码是否一致
        4. 生成 salt，将 salt + password 做哈希
        5. 插入 tbl_user
        6. 按 PDF 固定格式返回 JSON
    */
    server_.POST("/api/v1/auth/register", [](const HttpReq* req, HttpResp* resp) {
        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        string username = json_string(body, "username");
        string password = json_string(body, "password");
        string confirm = json_string(body, "confirm");

        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空"); // 400
            return;
        }

        if (password != confirm) {
            response_error(resp, HttpStatusBadRequest, "两次输入的密码不一致"); // 400
            return;
        }

        string salt = CryptoUtil::generate_salt();
        string password_hash = CryptoUtil::hash_password(password, salt);

        string sql =
            "INSERT INTO tbl_user (username, password, salt) VALUES ('" +
            escape_sql(username) + "', '" +
            escape_sql(password_hash) + "', '" +
            escape_sql(salt) + "');";

        cout << "[register SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp, username](MySQLResultCursor* cursor) {
           if (cursor->get_cursor_status() != MYSQL_STATUS_OK) {
               response_error(resp, HttpStatusConflict, "用户名已存在");
               return;
           }

           json data;
           data["userId"] = cursor->get_insert_id();
           data["username"] = username;
           response_success(resp, HttpStatusCreated, "注册成功", data);
        });
    });

    /*
        POST /api/v1/auth/login

        登录流程：
        1. 检查 JSON
        2. 按用户名查询用户
        3. 用数据库中的 salt 重新计算密码哈希
        4. 哈希一致则生成 JWT
    */
    server_.POST("/api/v1/auth/login", [](const HttpReq* req, HttpResp* resp) {
        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        string username = json_string(body, "username");
        string password = json_string(body, "password");

        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空"); // 400
            return;
        }

        string sql =
            "SELECT id, username, password, salt, created_at "
            "FROM tbl_user "
            "WHERE username='" + escape_sql(username) + "' AND tomb=0 "
            "LIMIT 1;";

        cout << "[login SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp, password](MySQLResultCursor* cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            if (cursor->get_rows_count() == 0) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误"); // 401
                return;
            }

            vector<MySQLCell> row;
            if (!cursor->fetch_row(row)) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误"); // 401
                return;
            }

            User user;
            user.id = row[0].as_int();
            user.username = row[1].as_string();
            user.password = row[2].as_string();
            user.salt = row[3].as_string();
            user.createdAt = row[4].as_string();

            string input_hash = CryptoUtil::hash_password(password, user.salt);
            if (input_hash != user.password) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误"); // 401
                return;
            }

            string token = CryptoUtil::generate_token(user);
            // 登陆成功时, generate_token(user) 会把用户的 id、username、createdAt 写入 JWT

            json data;
            data["accessToken"] = token;
            data["tokenType"] = "Bearer";
            data["user"]["userId"] = user.id;
            data["user"]["username"] = user.username;
            response_success(resp, HttpStatusOK, "登录成功", data);
        });
    });
}

void CloudDiskServer::register_user_module() // 用户信息获取模块
{
    server_.GET("/api/v1/user/me", [](const HttpReq* req, HttpResp* resp) {
        User user; // 传出参数, check_login 成功时, 会把 JWT 中的用户信息写入 user
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌"); // 401
            return;
        }

        json data;
        data["userId"] = user.id;
        data["username"] = user.username;
        data["createdAt"] = user.createdAt;
        response_success(resp, HttpStatusOK, "获取个人信息成功", data); // 200
    });
}

void CloudDiskServer::register_file_module() // 文件模块
{
    // 查询当前登录用户的文件列表
    server_.GET("/api/v1/files", [](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
            return;
        }

        string sql =
            "SELECT id, filename, size, created_at, last_update "
            "FROM tbl_file "
            "WHERE uid=" + to_string(user.id) + " "
            "ORDER BY last_update DESC, id DESC;";
            // 按最后更新时间倒序排列；如果更新时间一样，再按 id 倒序

        cout << "[list files SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp](MySQLResultCursor* cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            json files = json::array();
            vector<MySQLCell> row;

            // fetch_row(row) 每调用一次，就从结果集中读取下一行
            while (cursor->fetch_row(row)) {
                json file;
                file["fileId"] = row[0].as_int();
                file["filename"] = row[1].as_string();
                file["size"] = row[2].as_int();
                file["createdAt"] = row[3].as_string();
                file["updatedAt"] = row[4].as_string();
                files.push_back(file);
            }

            json data;
            data["files"] = files;
            response_success(resp, HttpStatusOK, "获取文件列表成功", data);
        });
    });

    /*
        POST /api/v1/files

        上传流程：
        1. 校验 token
        2. 检查 multipart/form-data
        3. 取出字段名为 file 的文件
        4. 根据文件内容生成 hashcode
        5. 写 tbl_file 记录
        6. 发布 RabbitMQ 消息，让后台消费者异步上传 OSS
    */
    /*
        第三期重构后把 OSS/RabbitMQ 能力封装成了 CloudDiskServer 的成员对象.
        lambda 想访问成员对象，就必须捕获 this.
        这里捕获 this，是为了让后续的异步 MySQL 回调能够继续访问当前
        CloudDiskServer 对象中的成员 oss_uploader_。
    */
    server_.POST("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌"); // 401
            return;
        }

        if (req->content_type() != MULTIPART_FORM_DATA) { // 上传文件必须使用 multipart/form-data
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        Form& form = req->form();
        /*
            req->form() 是 wfrest 对 multipart/form-data 请求体的解析结果。

            Form 的类型在 wfrest 里大致可以理解为：
                map<string, pair<string, string>>

            对于上传字段：
                form["file"].first  -> 文件名，例如 "a.txt"
                form["file"].second -> 文件内容，也就是 a.txt 的二进制内容

            这里使用引用 Form& form，是为了避免复制整份上传内容。
        */
        if (!form.count("file")) {
            // form 中没有 "file" 这个字段
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        string filename = form["file"].first;
        string content = form["file"].second;

        if (filename.empty()) { // 文件名不能为空
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());

        string sql =
            "INSERT INTO tbl_file (uid, filename, hashcode, size) VALUES (" +
            to_string(user.id) + ", '" +
            escape_sql(filename) + "', '" +
            escape_sql(hashcode) + "', " +
            to_string(content.size()) + ");";

        cout << "[upload SQL] " << sql << endl;

        /*
            MySQL 回调会在当前 HTTP 路由函数返回之后才执行。
            因此 filename/hashcode/content/uid 都必须按值捕获，避免局部变量失效。

            这里额外捕获 this，是因为回调里要调用：
                oss_uploader_.publish(...)

            oss_uploader_ 是 CloudDiskServer 的成员变量；lambda 只有捕获 this，
            才能通过当前对象访问这个成员。
        */
        resp->MySQL(DatabaseURL, sql, [resp,
                                       this,
                                       uid = user.id,
                                       filename,
                                       hashcode,
                                       content](MySQLResultCursor* cursor) {

            // INSERT 成功时 cursor 状态应为 MYSQL_STATUS_OK
            if (cursor->get_cursor_status() != MYSQL_STATUS_OK) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                    return;
            }

            // 数据库元数据写入成功后，再把 OSS 上传任务发布到 RabbitMQ
            // 这样用户感受到的上传接口耗时不再包含 OSS PutObject 的网络时间
            if (!oss_uploader_.publish(uid, hashcode, content)) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            json data;
            data["fileId"] = cursor->get_insert_id();
            data["filename"] = filename;
            response_success(resp, HttpStatusCreated, "上传成功", data);
        });
    });

    /*
        GET /api/v1/file/{id}

        下载流程：
        1. 校验 token
        2. 用 fileId + uid 查询文件记录
        3. 根据 uid + hashcode 从 OSS 下载对象内容
        4. 设置 Content-Disposition，让浏览器按原始文件名下载
        5. 返回 OSS 对象内容
    */
    /*
        这里捕获 this，是为了让下载接口内部的 MySQL 回调能够访问成员
        oss_storage_，从而调用 oss_storage_.download_object(...)。
    */
    server_.GET("/api/v1/file/{id}", [this](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌"); // 401
            return;
        }

        int file_id = req->param<int>("id"); // tbl_file 表中的文件 id
        string sql =
            "SELECT filename, hashcode "
            "FROM tbl_file "
            "WHERE id=" + to_string(file_id) + " AND uid=" + to_string(user.id) + " "
            "LIMIT 1;";

        cout << "[download SQL] " << sql << endl;

        /*
            这里捕获 this，是因为回调里要调用：
                oss_storage_.download_object(...)

            oss_storage_ 是 CloudDiskServer 的成员变量，不是普通局部变量。
            lambda 捕获 this 后，才能在异步回调中访问当前服务器对象的成员。
        */
        resp->MySQL(DatabaseURL, sql, [resp, uid = user.id, this](MySQLResultCursor* cursor) {
            // 只有 SELECT 正常执行并拿到结果集时，状态才是 MYSQL_STATUS_GET_RESULT
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            if (cursor->get_rows_count() == 0) { // 未查到
                response_error(resp, HttpStatusNotFound, "文件不存在"); // 404
                return;
            }

            vector<MySQLCell> row;
            if (!cursor->fetch_row(row)) {
                response_error(resp, HttpStatusNotFound, "文件不存在"); // 404
                return;
            }

            string filename = row[0].as_string();
            string hashcode = row[1].as_string();

            string content; // 这里的content是传出参数, 若download_object()执行成功, 其就保存了文件内容
            OssDownloadStatus status = oss_storage_.download_object(uid, hashcode, content);
            if (status == OssDownloadStatus::NotFound) {
                response_error(resp, HttpStatusNotFound, "文件不存在"); // 404
                return;
            }
            if (status == OssDownloadStatus::Failed) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            resp->set_status(HttpStatusOK);
            // 告诉浏览器响应体是通用二进制文件，不要当 JSON 或 HTML 解析
            resp->add_header("Content-Type", "application/octet-stream");
            // 告诉浏览器把响应体当附件下载，并附上文件名
            resp->add_header("Content-Disposition",
                             "attachment; filename=\"" +
                                 content_disposition_fallback_filename(filename) +
                                 "\"; filename*=" +
                                 encode_rfc5987_filename(filename));
            /*
                resp->String(...) 把内存中的文件内容写入 HTTP 响应体。

                move(content) 表示把 content 这块字符串内存“转交”给响应对象，
                尽量避免再复制一份文件内容。执行后本作用域不再使用 content。
            */
            resp->String(move(content));
        });
    });
}
