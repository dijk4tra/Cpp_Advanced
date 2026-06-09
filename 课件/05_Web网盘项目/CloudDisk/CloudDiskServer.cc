#include "CloudDiskServer.h"
#include "CryptoUtil.h"
#include "common.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <wfrest/PathUtil.h>
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
static const int RetryMax = 3;

// 第一阶段先把用户文件保存在本地磁盘。后续阶段接入 OSS 时，再替换这里的存储方式。
static const string FileStorageRoot = "./storage";

/*
    统一返回 JSON。

    前端 api.js 会读取 response.json()，并根据 status/message/data 判断结果。
    所以后端所有 API 错误都必须返回 JSON，不能返回普通文本或 HTML。
*/
static void response_json(HttpResp* resp, int status_code, const json& body)
{
    resp->set_status(status_code);
    resp->add_header("Content-Type", "application/json");
    resp->String(body.dump());
}

/*
    成功响应的公共格式：
    {
        "status": "success",
        "message": "...",
        "data": { ... }
    }
*/
static void response_success(HttpResp* resp, int status_code, const string& message, const json& data)
{
    json body;
    body["status"] = "success";
    body["message"] = message;
    body["data"] = data;
    response_json(resp, status_code, body);
}

/*
    失败响应的公共格式：
    {
        "status": "error",
        "message": "..."
    }
*/
static void response_error(HttpResp* resp, int status_code, const string& message)
{
    json body;
    body["status"] = "error";
    body["message"] = message;
    response_json(resp, status_code, body);
}

/*
    简单处理 SQL 字符串中的单引号和反斜线。

    例如用户名是 abc'def，如果直接拼到 SQL 中：
        'abc'def'
    SQL 字符串会被提前截断。

    正式项目应该使用参数化查询；这里是学习项目，先用这个函数把拼接 SQL 的风险降下来。
*/
static string escape_sql(const string& s)
{
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

/*
    Content-Disposition 的 filename 需要放到响应头里。
    这里只做最基础的双引号和反斜线转义，避免文件名破坏响应头格式。
*/
static string escape_header_filename(const string& filename)
{
    string result;
    for (char ch : filename) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
        }
        result += ch;
    }
    return result;
}

/*
    解析 JSON 请求体。

    注册和登录都要求 Content-Type: application/json。
    nlohmann::json::parse(..., false) 在解析失败时不会抛异常，而是返回 discarded 状态，
    对教学项目来说，这样的错误处理更直观。
*/
static bool parse_json_body(const HttpReq* req, json& body)
{
    if (req->content_type() != APPLICATION_JSON) {
        return false;
    }

    body = json::parse(req->body(), nullptr, false);
    return !body.is_discarded();
}

/*
    从 JSON 中读取字符串字段。

    如果字段不存在或字段类型不是字符串，就返回空字符串。
    这样接口代码里只要判断 empty()，流程会比较清楚。
*/
static string json_string(const json& body, const string& key)
{
    if (!body.contains(key) || !body[key].is_string()) {
        return "";
    }
    return body[key].get<string>();
}

/*
    从 Authorization 头中取 Bearer Token。

    请求头必须是：
        Authorization: Bearer xxxxxx
    只要没有令牌、类型不是 Bearer、令牌为空，都按 PDF 要求返回“无效的访问令牌”。
*/
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

/*
    校验登录态。

    后续需要登录的接口都先调用这个函数：
    1. 解析 Bearer Token
    2. 调用 CryptoUtil::verify_token 校验签名和过期时间
    3. 校验成功后，把 token 中的用户信息填入 user
*/
static bool check_login(const HttpReq* req, User& user)
{
    string token;
    if (!get_bearer_token(req, token)) {
        return false;
    }

    return CryptoUtil::verify_token(token, user);
}

/*
    为每个用户准备单独的本地存储目录。

    例如 uid=3 的用户，文件保存到：
        ./storage/3/文件hash
*/
static string user_storage_dir(int uid)
{
    return FileStorageRoot + "/" + to_string(uid);
}

/*
    用 uid 和 hashcode 拼出本地文件路径。

    数据库 tbl_file 保存 filename 和 hashcode：
    - filename：给用户看的原始文件名
    - hashcode：后端保存文件时使用的文件名
*/
static string storage_file_path(int uid, const string& hashcode)
{
    return user_storage_dir(uid) + "/" + hashcode;
}

void CloudDiskServer::register_routes()
{
    // 设置静态资源的路由
    register_www_module();
    register_auth_module();
    register_user_module();
    register_file_module();
    // ...
}

void CloudDiskServer::register_www_module()
{
    server_.Static("/", "./www/index.html");
    server_.Static("/static", "./www/static");
}

void CloudDiskServer::register_auth_module()
{
    /*
        POST /api/v1/auth/register

        注册流程：
        1. 检查请求类型必须是 JSON
        2. 读取 username/password/confirm
        3. 检查空值和两次密码是否一致
        4. 生成 salt，将 salt + password 做哈希
        5. 插入 tbl_user
        6. 按 PDF 固定格式返回 JSON
    */
    server_.POST("/api/v1/auth/register", [](const HttpReq* req, HttpResp* resp) {
        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        string username = json_string(body, "username");
        string password = json_string(body, "password");
        string confirm = json_string(body, "confirm");

        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空");
            return;
        }

        if (password != confirm) {
            response_error(resp, HttpStatusBadRequest, "两次输入的密码不一致");
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

        /*
            注意：MySQL 查询是异步执行的。
            Lambda 捕获 username 是按值捕获，避免当前函数返回后局部变量失效。
        */
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
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        string username = json_string(body, "username");
        string password = json_string(body, "password");

        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空");
            return;
        }

        string sql =
            "SELECT id, username, password, salt, created_at "
            "FROM tbl_user "
            "WHERE username='" + escape_sql(username) + "' AND tomb=0 "
            "LIMIT 1;";

        cout << "[login SQL] " << sql << endl;

        /*
            password 必须按值捕获。
            因为数据库回调执行时，外层 login 回调早已返回，引用捕获会变成悬空引用。
        */
        resp->MySQL(DatabaseURL, sql, [resp, password](MySQLResultCursor* cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            if (cursor->get_rows_count() == 0) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误");
                return;
            }

            vector<MySQLCell> row;
            if (!cursor->fetch_row(row)) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误");
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
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误");
                return;
            }

            string token = CryptoUtil::generate_token(user);

            json data;
            data["accessToken"] = token;
            data["tokenType"] = "Bearer";
            data["user"]["userId"] = user.id;
            data["user"]["username"] = user.username;
            response_success(resp, HttpStatusOK, "登录成功", data);
        });
    });
}

void CloudDiskServer::register_user_module()
{
    /*
        GET /api/v1/user/me

        第一阶段的 token 中已经保存了 userId、username、createdAt。
        所以当前用户信息可以直接从 token 中取，不需要再查数据库。
    */
    server_.GET("/api/v1/user/me", [](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
            return;
        }

        json data;
        data["userId"] = user.id;
        data["username"] = user.username;
        data["createdAt"] = user.createdAt;
        response_success(resp, HttpStatusOK, "获取个人信息成功", data);
    });
}

void CloudDiskServer::register_file_module()
{
    /*
        GET /api/v1/files

        查询当前登录用户的文件列表。
        注意：uid 来自 token，而不是来自前端参数。
        这样用户只能看到自己的文件。
    */
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

        cout << "[list files SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp](MySQLResultCursor* cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            json files = json::array();
            vector<MySQLCell> row;
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
        5. 保存文件到本地 storage 目录
        6. 写 tbl_file 记录
    */
    server_.POST("/api/v1/files", [](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
            return;
        }

        if (req->content_type() != MULTIPART_FORM_DATA) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        Form& form = req->form();
        if (!form.count("file")) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        string filename = form["file"].first;
        string content = form["file"].second;
        if (filename.empty()) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());
        string dir = user_storage_dir(user.id);
        string real_path = storage_file_path(user.id, hashcode);

        /*
            create_directories 会递归创建目录。
            目录已经存在时，它不会报错，适合上传前准备用户目录。
        */
        std::error_code ec;
        filesystem::create_directories(dir, ec);
        if (ec) {
            response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
            return;
        }

        /*
            第一阶段文件比较小，直接把 multipart 中的内容写入本地文件即可。
            第二阶段接入 OSS 后，可以把这一步替换成上传到对象存储。
        */
        ofstream ofs(real_path, ios::binary);
        if (!ofs) {
            response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
            return;
        }
        ofs.write(content.data(), content.size());
        ofs.close();

        string sql =
            "INSERT INTO tbl_file (uid, filename, hashcode, size) VALUES (" +
            to_string(user.id) + ", '" +
            escape_sql(filename) + "', '" +
            escape_sql(hashcode) + "', " +
            to_string(content.size()) + ");";

        cout << "[upload SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp, filename](MySQLResultCursor* cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_OK) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
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
        3. 根据 hashcode 找到本地文件
        4. 设置 Content-Disposition，让浏览器按原始文件名下载
        5. resp->File() 发送文件内容
    */
    server_.GET("/api/v1/file/{id}", [](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
            return;
        }

        int file_id = req->param<int>("id");
        string sql =
            "SELECT filename, hashcode "
            "FROM tbl_file "
            "WHERE id=" + to_string(file_id) + " AND uid=" + to_string(user.id) + " "
            "LIMIT 1;";

        cout << "[download SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp, uid = user.id](MySQLResultCursor* cursor) {
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            if (cursor->get_rows_count() == 0) {
                response_error(resp, HttpStatusNotFound, "文件不存在");
                return;
            }

            vector<MySQLCell> row;
            if (!cursor->fetch_row(row)) {
                response_error(resp, HttpStatusNotFound, "文件不存在");
                return;
            }

            string filename = row[0].as_string();
            string hashcode = row[1].as_string();
            string real_path = storage_file_path(uid, hashcode);

            if (!filesystem::exists(real_path) || !filesystem::is_regular_file(real_path)) {
                response_error(resp, HttpStatusNotFound, "文件不存在");
                return;
            }

            resp->set_status(HttpStatusOK);
            resp->add_header("Content-Disposition",
                             "attachment; filename=\"" + escape_header_filename(filename) + "\"");
            resp->File(real_path);
        });
    });
}
