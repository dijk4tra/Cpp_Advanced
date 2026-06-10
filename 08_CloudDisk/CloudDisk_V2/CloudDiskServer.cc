#include "CloudDiskServer.h"
#include "CryptoUtil.h"
#include "common.h"
#include <alibabacloud/oss/OssClient.h>
#include <alibabacloud/oss/client/ClientConfiguration.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
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
namespace oss = AlibabaCloud::OSS;

// 数据库的URL
static const string DatabaseURL = "mysql://root:123456@localhost/CloudDisk";
static const int RetryMax = 3;

/*
    OSS 连接配置。

    第二阶段不再把文件写到服务器本地磁盘，而是写入阿里云对象存储 OSS：
    - endpoint/region/bucket 来自当前使用的 Bucket 所在地域
    - accessKeyId/accessKeySecret 用来给 SDK 请求签名

    这里从.env文件中获取accessKeyId/accessKeySecret等信息
*/

static string getEnvOrThrow(const char* name) {
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        throw runtime_error(string("Missing environment variable: ") + name);
    }
    return string(value);
}

static const string OssEndpoint = getEnvOrThrow("ALIBABA_CLOUD_OSS_ENDPOINT");
static const string OssAccessKeyId = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_ID");
static const string OssAccessKeySecret = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
static const string OssBucketName = getEnvOrThrow("ALIBABA_CLOUD_OSS_BUCKET");
static const string OssRegion = getEnvOrThrow("ALIBABA_CLOUD_OSS_REGION");

// 后端统一返回 JSON 给前端
static void response_json(HttpResp* resp, int status_code, const json& body)
{
    resp->set_status(status_code);
    resp->add_header("Content-Type", "application/json");
    resp->String(body.dump());
}

/*
    成功响应的 Json 格式：
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
    失败响应的 Json 格式：
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

// 简单处理 SQL 字符串中的单引号和反斜线
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

// Content-Disposition 的 filename 需要放到响应头里
// 这里只做最基础的双引号和反斜线转义，避免文件名破坏响应头格式
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
    这样的错误处理更直观
*/
static bool parse_json_body(const HttpReq* req, json& body)
{
    if (req->content_type() != wfrest::APPLICATION_JSON) {
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
    只要没有令牌、类型不是 Bearer、令牌为空，都返回“无效的访问令牌”。
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
    创建 OSS 客户端。

    OssClient 内部保存 endpoint、AccessKey 和连接配置。
    这里每次上传/下载都创建一个轻量客户端对象，避免把客户端做成全局静态对象：
    - 全局静态对象的析构顺序不直观，容易在 ShutdownSdk() 之后才析构
    - 当前项目请求量较小，按请求创建客户端更容易理解，也足够满足教学项目需求

    注意：真正的 SDK 网络资源由 InitializeSdk()/ShutdownSdk() 统一管理，
    它们在 CloudDiskServer 构造/析构中各执行一次。
*/
static unique_ptr<oss::OssClient> create_oss_client()
{
    oss::ClientConfiguration conf;
    auto client = make_unique<oss::OssClient>(
        OssEndpoint,
        OssAccessKeyId,
        OssAccessKeySecret,
        conf);
    client->SetRegion(OssRegion);
    return client;
}

/*
    生成 OSS ObjectName。

    OSS 中没有真正的目录，"users/3/abc123" 这样的路径只是对象名的一部分。
    按 uid 分前缀有两个好处：
    1. 不同用户的同一 hash 文件不会互相覆盖
    2. 后续如果要按用户清理文件，可以直接按 users/{uid}/ 前缀列举对象

    数据库仍然保存：
    - filename：用户上传时的原始文件名，用于列表展示和下载文件名
    - hashcode：文件内容 hash，用于定位 OSS 中的对象
*/
static string oss_object_name(int uid, const string& hashcode)
{
    return "users/" + to_string(uid) + "/" + hashcode;
}

/*
    统一打印 OSS SDK 错误。

    HTTP 接口不能把 AccessKey、Bucket 内部信息等细节返回给前端；
    但服务端日志需要保留 OSS 返回的 code/message/requestId，
    这样排查权限、地域、Bucket 名称、网络问题时有依据。
*/
static void log_oss_error(const string& action, const oss::OssError& error)
{
    cerr << "[OSS " << action << " FAILED]"
         << " code:" << error.Code()
         << ", message:" << error.Message()
         << ", requestId:" << error.RequestId()
         << endl;
}

/*
    把上传文件内容写入 OSS。

    wfrest 已经把 multipart/form-data 中的文件内容解析到 string content。
    OSS C++ SDK 的 PutObjectRequest 需要 shared_ptr<iostream>，
    所以这里把 string 放进 stringstream，再交给 SDK 上传。
*/
static bool oss_upload_object(int uid, const string& hashcode, const string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object_name = oss_object_name(uid, hashcode);

    auto stream = make_shared<stringstream>(ios::in | ios::out | ios::binary);
    stream->write(content.data(), content.size());
    stream->seekg(0);

    oss::PutObjectRequest request(bucket_name, object_name, stream);
    auto outcome = client->PutObject(request);
    if (!outcome.isSuccess()) {
        log_oss_error("PutObject", outcome.error());
        return false;
    }
    return true;
}

enum class OssDownloadStatus {
    Ok,
    NotFound,
    Failed
};

/*
    从 OSS 下载对象内容。

    数据库中能查到文件记录，只代表“元数据存在”；真正的文件内容还要去 OSS 取。
    如果 OSS 返回 NoSuchKey，说明对象存储里已经没有这个对象了，对外按“文件不存在”处理。
    其它错误通常是权限、网络、地域、Bucket 配置等服务端问题，对外按 500 处理。
*/
static OssDownloadStatus oss_download_object(int uid,
                                             const string& hashcode,
                                             string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object_name = oss_object_name(uid, hashcode);

    auto outcome = client->GetObject(bucket_name, object_name);
    if (!outcome.isSuccess()) {
        if (outcome.error().Code() == "NoSuchKey") {
            return OssDownloadStatus::NotFound;
        }
        log_oss_error("GetObject", outcome.error());
        return OssDownloadStatus::Failed;
    }

    auto stream = outcome.result().Content();
    if (!stream) {
        cerr << "[OSS GetObject FAILED] empty content stream" << endl;
        return OssDownloadStatus::Failed;
    }

    ostringstream oss_content;
    oss_content << stream->rdbuf();
    content = oss_content.str();
    return OssDownloadStatus::Ok;
}

CloudDiskServer::CloudDiskServer()
{
    /*
        OSS C++ SDK 要求 InitializeSdk() 在使用任何 OSS API 之前调用。
        这个服务器对象在 main() 中只创建一次，所以把 SDK 初始化放在构造函数中，
        可以保证整个进程生命周期内只初始化一次。
    */
    oss::InitializeSdk();
}

CloudDiskServer::~CloudDiskServer()
{
    /*
        ShutdownSdk() 释放 SDK 内部网络、内存等全局资源。
        main() 中会先 stop() 服务器，再让 CloudDiskServer 析构，
        因此这里执行释放时已经不会再处理新的 OSS 请求。
    */
    oss::ShutdownSdk();
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

/*
    注册和登陆
*/
void CloudDiskServer::register_auth_module()
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
            /*
                INSERT/UPDATE/DELETE 这类“不返回结果集”的 SQL，成功时状态通常是 MYSQL_STATUS_OK
                如果状态不是 MYSQL_STATUS_OK，说明这条插入语句没有被 MySQL 正常执行完成
            */
            if (cursor->get_cursor_status() != MYSQL_STATUS_OK) {
                response_error(resp, HttpStatusConflict, "用户名已存在"); // 409
                return;
            }

            json data;
            data["userId"] = cursor->get_insert_id();
            data["username"] = username;
            response_success(resp, HttpStatusCreated, "注册成功", data); // 201
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

        cout << "[Login SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp, password](MySQLResultCursor* cursor) { // password必须按值捕获
            /*
                对 SELECT 来说，正常情况应该得到一个“结果集”，wfrest/workflow 用
                MYSQL_STATUS_GET_RESULT 表示“SQL 执行成功，并且 cursor 中有可读取的结果集”
                如果状态不是 MYSQL_STATUS_GET_RESULT，说明是 SQL 执行过程本身出了问题,
                例如数据库连接失败, SQL 语法错误, 表名/字段名写错等
            */
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            /*
                走到这里，说明 SELECT 语句本身执行成功了
                get_rows_count() 表示这个 SELECT 查询到了多少行记录

                如果 rows_count == 0，说明数据库中没有这个可用用户：
                - 用户名不存在
                - 或者用户已被逻辑删除(tomb != 0)
                从登录接口角度看，这就等价于“用户名或密码错误”。
            */
            if (cursor->get_rows_count() == 0) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误"); // 401
                return;
            }

            vector<MySQLCell> row;
            /*
                fetch_row(row) 的作用是：把当前结果集中的“下一行”真正取出来，
                并放入 row 这个 vector<MySQLCell> 中。
                在当前登录场景里，fetch_row 失败同样意味着没有拿到可用于登录的用户记录。
            */
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
            // 数据库里存的不是明文密码，而是 hash_password(原始密码, salt) 的结果
            if (input_hash != user.password) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误"); // 401
                return;
            }

            string token = CryptoUtil::generate_token(user);

            json data;
            data["accessToken"] = token;
            data["tokenType"] = "Bearer ";
            data["user"]["userId"] = user.id;
            data["user"]["username"] = user.username;
            response_success(resp, HttpStatusOK, "登陆成功", data); // 200
        });
    });
}

/*
    获取当前用户信息
*/
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
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌"); // 401
            return;
        }

        json data;
        data["userId"] = user.id;
        data["username"] = user.username;
        data["createdAt"] = user.createdAt;
        response_success(resp, HttpStatusOK, "获取个人信息成功", data);
    });
}

/*
    文件列表查询
*/
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
           response_error(resp, HttpStatusUnauthorized, "无效的访问令牌"); // 401
       }

       string sql =
           "SELECT id, filename, size, created_at, last_update "
           "FROM tbl_file "
           "WHERE uid=" + to_string(user.id) + " "
           "ORDER BY last_update DESC, id DESC;"; // 按最后更新时间倒序排列；如果更新时间一样，再按 id 倒序。

       cout << "[list files SQL] " << sql << endl;

       resp->MySQL(DatabaseURL, sql, [resp](MySQLResultCursor* cursor) {
           /*
               文件列表接口执行的是 SELECT。
               SELECT 成功时必须得到 MYSQL_STATUS_GET_RESULT。
               如果不是这个状态，说明 SQL 没有正常执行完成，
               例如数据库连接失败、tbl_file 表不存在、字段名写错等。
               这些都是服务端问题，所以返回 500 + “内部服务器错误”。
           */
           if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
               response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
               return;
           }

           json files = json::array();
           vector<MySQLCell> row;
           /*
               fetch_row(row) 每调用一次，就从结果集中读取下一行。

               对文件列表来说，“查不到任何文件”不是错误，而是一个正常状态：
                   files = []
               所以这里不需要先判断 get_rows_count() == 0。
               如果没有文件，while 循环一次都不进，最后返回空数组即可。
           */
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
        上传文件
        POST /api/v1/files

        上传流程：
        1. 校验 token
        2. 检查 multipart/form-data
        3. 取出字段名为 file 的文件
        4. 根据文件内容生成 hashcode
        5. 上传文件内容到 OSS
        6. 写 tbl_file 记录
    */
    server_.POST("/api/v1/files", [](const HttpReq* req, HttpResp* resp) {
        User user;
        if (!check_login(req, user)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
            return;
        }

        /*
            上传文件必须使用 multipart/form-data

            前端 api.js 里 uploadRequest() 使用的是 FormData：
                const formData = new FormData();
                formData.append('file', file);

            浏览器发送 FormData 时，请求头会类似：
                Content-Type: multipart/form-data; boundary=----WebKitFormBoundary...

            如果 content_type 不是 MULTIPART_FORM_DATA，说明客户端没有按“文件上传表单”的格式发请求，
            wfrest 也无法按文件表单解析 body，所以按照 PDF 要求返回 400 + “请求格式有误”。
        */
        if (req->content_type() != wfrest::MULTIPART_FORM_DATA) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            req->form() 是 wfrest 对 multipart/form-data 请求体的解析结果。

            Form 的类型在 wfrest 里大致可以理解为：
                map<string, pair<string, string>>

            对于上传字段：
                form["file"].first  -> 文件名，例如 "a.txt"
                form["file"].second -> 文件内容，也就是 a.txt 的二进制内容

            这里使用引用 Form& form，是为了避免复制整份上传内容。
            如果上传的是 10MB 文件，复制一份 form 就会多占一份内存。
        */
        Form& form = req->form();
        /*
            前端固定使用字段名 file：
                formData.append('file', file);

            如果 form 中没有 "file" 这个字段，说明客户端虽然用了 multipart/form-data，
            但没有按接口约定上传文件字段。后端无法知道哪个 part 才是要保存的文件，
            所以这是请求格式错误，返回 400 + “请求格式有误”。
        */
        if (!form.count("file")) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        string filename = form["file"].first;
        string content = form["file"].second;
        /*
            form["file"].first 是 multipart 中 Content-Disposition 里的 filename。

            正常文件上传会带文件名，例如：
                Content-Disposition: form-data; name="file"; filename="a.txt"

            filename 为空时，后端无法给文件生成用户可见的名称，所以返回 400 + “请求格式有误”。
        */
        if (filename.empty()) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误"); // 400
            return;
        }

        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size()); // 文件内容哈希
        /*
            第二阶段文件内容不再落到服务器本地磁盘，而是上传到 OSS。

            oss_upload_object() 内部会把对象保存成：
                users/{uid}/{hashcode}

            例如：
                user.id   = 3
                hashcode  = "abc123..."
                object    = "users/3/abc123..."
        */
        if (!oss_upload_object(user.id, hashcode, content)) {
            response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
            return;
        }

        string sql =
            "INSERT INTO tbl_file (uid, filename, hashcode, size) VALUES (" +
            to_string(user.id) + ", '" +
            escape_sql(filename) + "', '" +
            escape_sql(hashcode) + "', " +
            to_string(content.size()) + ");";

        cout << "[upload SQL] " << sql << endl;

        resp->MySQL(DatabaseURL, sql, [resp, filename](MySQLResultCursor* cursor) {
            /*
                上传接口最后执行的是 INSERT。
                INSERT 成功时 cursor 状态应为 MYSQL_STATUS_OK。

                如果不是 MYSQL_STATUS_OK，说明文件元数据没有成功写入 tbl_file。
                常见原因包括：
                - 数据库连接失败
                - tbl_file 表或字段不存在
                - 同一用户上传了同名文件，触发 UNIQUE KEY (uid, filename)

                PDF 对上传阶段只要求“SQL 语句执行失败”返回 500，
                所以这里不再细分原因，统一返回“内部服务器错误”。
            */
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
        文件下载
        GET /api/v1/file/{id}

        下载流程：
        1. 校验 token
        2. 用 fileId + uid 查询文件记录
        3. 根据 uid + hashcode 从 OSS 下载对象内容
        4. 设置 Content-Disposition，让浏览器按原始文件名下载
        5. 返回 OSS 对象内容
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
            /*
                下载前需要先 SELECT 文件记录。
                只有 SELECT 正常执行并拿到结果集时，状态才是 MYSQL_STATUS_GET_RESULT。

                如果状态不是 MYSQL_STATUS_GET_RESULT，说明 SQL 执行失败，
                这属于服务器端或数据库端问题，返回 500。
            */
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            if (cursor->get_rows_count() == 0) { // 没有找到对应的文件记录
                response_error(resp, HttpStatusNotFound, "文件不存在"); // 404
                return;
            }

            vector<MySQLCell> row;
            if (!cursor->fetch_row(row)) { // 无法取出第一条数据
                response_error(resp, HttpStatusNotFound, "文件不存在"); // 404
                return;
            }

            string filename = row[0].as_string();
            string hashcode = row[1].as_string();

            string content; // 这里的content是传出参数, 若oss_download_object()执行成功, 其就保存了文件内容
            OssDownloadStatus status = oss_download_object(uid, hashcode, content);
            if (status == OssDownloadStatus::NotFound) {
                response_error(resp, HttpStatusNotFound, "文件不存在"); // 404
                return;
            }
            if (status == OssDownloadStatus::Failed) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误"); // 500
                return;
            }

            // 设置响应状态码
            resp->set_status(HttpStatusOK);
            // 设置响应头
            resp->add_header("Content-Type", "application/octet-stream"); // 告诉客户端这是一个二进制文件
            resp->add_header("Content-Disposition",
                             "attachment; filename=\"" + escape_header_filename(filename) + "\""); // 告诉客户端这是一个附件, 并指定文件名
            resp->String(move(content)); // 把内存中的文件内容写入 HTTP 响应体
        });
    });
}
