#include "CloudDiskServer.h"
#include "../common/CryptoUtil.h"
#include "../common/OssStorage.h"

#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <wfrest/PathUtil.h>
#include <workflow/HttpUtil.h>

using namespace std;
using namespace wfrest;
using json = nlohmann::json;

/*
    给 protobuf 生成的命名空间起一个短别名。
    这样后面写 pb::LoginRequest 比 cloud::disk::LoginRequest 更短。
*/
namespace pb = cloud::disk;

// 读取字符串环境变量
static string get_env_or_default(const char* name, const string& default_value)
{
    /*
        getenv() 返回环境变量字符串。
        变量不存在时返回 nullptr。
    */
    const char* value = getenv(name);

    if (!value || string(value).empty()) {
        // 未配置或配置为空时使用默认值
        return default_value;
    }

    return string(value);
}

// 读取端口环境变量
static unsigned short get_env_port(const char* name, unsigned short default_port)
{
    const char* value = getenv(name);

    // 没有配置就使用默认端口
    if (!value || string(value).empty()) {
        return default_port;
    }

    // 把字符串转成长整数
    char* end = nullptr;
    long port = strtol(value, &end, 10);

    // 如果字符串不是纯数字，或者端口超出合法范围，就回退到默认端口
    if (*end != '\0' || port <= 0 || port > 65535) {
        return default_port;
    }

    return static_cast<unsigned short>(port);
}

/*
    三个后端微服务的地址。

    第四期先用本地固定默认值：
    - AuthService: 9001
    - UserService: 9002
    - FileMetaService: 9003

    第五期引入 Consul 后，网关就不需要硬编码这些地址。
*/
static const string AuthServiceHost = get_env_or_default("AUTH_SERVICE_HOST", "127.0.0.1");
static const unsigned short AuthServicePort = get_env_port("AUTH_SERVICE_PORT", 9001);

static const string UserServiceHost = get_env_or_default("USER_SERVICE_HOST", "127.0.0.1");
static const unsigned short UserServicePort = get_env_port("USER_SERVICE_PORT", 9002);

static const string FileMetaServiceHost = get_env_or_default("FILEMETA_SERVICE_HOST", "127.0.0.1");
static const unsigned short FileMetaServicePort = get_env_port("FILEMETA_SERVICE_PORT", 9003);

/*
    统一返回 JSON。

    前端 api.js 会调用 response.json()，所以 API 错误也必须返回 JSON，
    不能返回普通文本或 HTML。
*/
static void response_json(HttpResp* resp, int status_code, const json& body)
{
    resp->set_status(status_code); // 设置 HTTP 状态码
    resp->add_header("Content-Type", "application/json"); // 告诉浏览器响应体是 JSON
    resp->String(body.dump()); // nlohmann::json::dump() 把 JSON 对象序列化成字符串
}

/*
    成功响应公共格式：
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
    失败响应公共格式：
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
    将 RPC 业务错误码转换成 HTTP 状态码。

    当前 proto 中约定业务错误码直接使用 400/401/404/409/500，
    所以大部分情况直接返回 code 即可。
*/
static int rpc_code_to_http_status(int code)
{
    if (code == 400 || code == 401 || code == 404 || code == 409 || code == 500) {
        return code;
    }
    // 未知错误按内部服务器错误处理, 统一返回500
    return HttpStatusInternalServerError;
}

/*
    处理 RPC 通信层失败。
    ctx->success() == false 说明不是业务错误，而是 RPC 网络/序列化/服务不可达等问题。
*/
static bool check_rpc_context(HttpResp* resp, srpc::RPCContext* ctx)
{
    //  RPC 成功到达服务端并拿到响应时，success() 为 true
    if (ctx->success()) {
        return true;
    }

    // 把底层错误打印到服务端日志，方便排查后端服务是否启动
    cerr << "[RPC FAILED] error=" << ctx->get_error()
         << ", msg=" << ctx->get_errmsg() << endl;

    // 对前端统一返回内部服务器错误，不暴露内部地址和 RPC 细节
    response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
    return false; // false 表示调用方不能继续读取业务响应
}

// 解析 JSON 请求体
static bool parse_json_body(const HttpReq* req, json& body)
{
    // 注册和登录都要求 application/json
    if (req->content_type() != APPLICATION_JSON) {
        return false;
    }

    // parse(..., false) 表示解析失败时不抛异常，而是返回 discarded
    body = json::parse(req->body(), nullptr, false);
    return !body.is_discarded(); // 不是 discarded 就说明 JSON 语法合法
}

// 从 JSON 中读取字符串字段
static string json_string(const json& body, const string& key)
{
    // 字段不存在或类型不是字符串时，返回空字符串
    if (!body.contains(key) || !body[key].is_string()) {
        return "";
    }
    // 字段存在且是字符串，取出它
    return body[key].get<string>();
}

// 从 Authorization 头中取 Bearer Token
static bool get_bearer_token(const HttpReq* req, string& token)
{
    if (!req->has_header("Authorization")) {
        // 没有 Authorization 头，说明没有携带登录态
        return false;
    }

    // 读取完整 Authorization 头
    const string& authorization = req->header("Authorization");
    const string prefix = "Bearer "; // 只接受 Bearer Token

    if (authorization.size() <= prefix.size()) {
        // 长度不够时，不可能包含有效 token
        return false;
    }

    if (authorization.substr(0, prefix.size()) != prefix) {
        // 前缀必须严格等于 "Bearer "
        return false;
    }

    // 截掉 Bearer 前缀，得到真正的 token
    token = authorization.substr(prefix.size());
    return !token.empty(); // token 不能为空
}

// 生成 Content-Disposition 中 filename= 使用的 ASCII 兜底文件名
static string content_disposition_fallback_filename(const string& filename)
{
    string result;

    /*
        按字节扫描文件名。
        中文 UTF-8 字节会被替换成 '_'，避免老浏览器乱码。
    */
    for (unsigned char ch : filename) {
        // 双引号和反斜线在 HTTP header 参数中需要转义
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += ch;
        // 可打印 ASCII 字符可以直接放进 filename=
        } else if (ch >= 0x20 && ch <= 0x7e) {
            result += ch;
        // 非 ASCII 字节统一替换成下划线
        } else {
            result += '_';
        }
    }
    // 如果最终为空，给一个默认文件名
    return result.empty() ? "download" : result;
}

// 判断字符是否可以直接出现在 RFC 5987 filename* 参数中
static bool is_rfc5987_attr_char(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '!' || ch == '#' || ch == '$' ||
           ch == '&' || ch == '+' || ch == '-' ||
           ch == '.' || ch == '^' || ch == '_' ||
           ch == '`' || ch == '|' || ch == '~';
}

// 把 UTF-8 文件名编码成 filename* 可用的格式
static string encode_rfc5987_filename(const string& filename)
{
    static const char* hex = "0123456789ABCDEF"; // 十六进制字符表，用于百分号编码
    string result = "UTF-8''"; // filename* 需要带上字符集前缀

    // 按 UTF-8 字节逐个处理
    for (unsigned char ch : filename) {
        if (is_rfc5987_attr_char(ch)) {
            // 允许直接出现的字符不编码
            result += ch;
        } else {
            result += '%';
            result += hex[ch >> 4];
            result += hex[ch & 0x0f];
        }
    }

    return result;
}

/*
    统一的鉴权入口
    根据 token 字符串异步调用 AuthService.VerifyToken
    成功后继续执行业务回调

    参数：
    - token：已经从 HTTP Header 中解析出来的 Bearer Token。
    - resp：当前 HTTP 响应对象。
    - auth_client：AuthService srpc 客户端。
    - next：token 校验成功后要继续执行的业务逻辑。
*/
static void verify_token_async(
    const string& token,
    HttpResp* resp,
    pb::AuthService::SRPCClient& auth_client,
    const function<void(pb::UserIdentity)>& next)
{
    /*
        创建 VerifyToken RPC task。
        这个 task 启动后会异步访问 AuthService。
    */
    srpc::SRPCClientTask* task =
        auth_client.create_VerifyToken_task(
            [resp, next](pb::VerifyTokenResponse* rpc_resp, srpc::RPCContext* ctx) {

                if (!check_rpc_context(resp, ctx)) { // 先判断 RPC 通信层是否成功
                    return;
                }

                int code = rpc_resp->result().code(); // 读取业务结果码

                if (code != 0) { // code != 0 表示 token 无效或服务端业务错误
                    response_error(resp,
                                   rpc_code_to_http_status(rpc_resp->result().code()),
                                   rpc_resp->result().message());
                    return;
                }

                /*
                    token 校验成功，把用户身份传给下一步业务。
                    这里按值传递，避免 rpc_resp 回调结束后对象失效。
                */
                next(rpc_resp->user());
            });

    pb::VerifyTokenRequest rpc_req;  // 构造 protobuf 请求
    rpc_req.set_access_token(token); // 写入 token 字符串
    task->serialize_input(&rpc_req); // serialize_input 会把请求序列化进 srpc task

    /*
        关键点：这里不能直接 task->start()。

        wfrest 的 HTTP 处理函数返回后，如果没有把异步任务接到当前 HTTP 请求的
        SeriesWork 上，框架会认为响应已经结束，浏览器就会收到空响应。

        resp->add_task(task) 会把 srpc task 加入当前 HTTP 响应所在的 workflow 序列，
        等 RPC 回调写完 resp 后，HTTP 响应才真正结束。
    */
    resp->add_task(task);
}

// 从 HTTP 请求头读取 token，并调用 verify_token_async
static void verify_request_async(
    const HttpReq* req,
    HttpResp* resp,
    pb::AuthService::SRPCClient& auth_client,
    const function<void(pb::UserIdentity)>& next)
{
    string token; // 用来保存解析出的 Bearer Token

    if (!get_bearer_token(req, token)) {
        // 请求没有携带合法 Bearer Token，直接返回 401
        response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
        return;
    }

    // token 字符串已经拷贝出来，后续异步回调不再依赖 HttpReq 生命周期
    verify_token_async(token, resp, auth_client, next);
}

CloudDiskServer::CloudDiskServer()
    : oss_uploader_()
    , auth_client_(AuthServiceHost.c_str(), AuthServicePort)
    , user_client_(UserServiceHost.c_str(), UserServicePort)
    , filemeta_client_(FileMetaServiceHost.c_str(), FileMetaServicePort)
{
    /*
        第四期开始，API Gateway 不再启动 RabbitMQ 消费线程。

        这里仍然保留 oss_uploader_ 成员，是因为网关还需要：
        - 保存上传临时文件
        - 删除无用临时文件
        - 发布 RabbitMQ 上传任务

        真正消费 RabbitMQ 并上传 OSS 的工作，交给独立 oss_upload_worker 进程。
    */
}

CloudDiskServer::~CloudDiskServer() = default;
/*
    网关没有启动 RabbitMQ 消费线程，所以析构时不需要 stop()。
    RabbitMqOssUploader 析构函数内部也会兜底调用 stop()，即使没启动也安全。
*/

void CloudDiskServer::register_routes()
{
    register_www_module();  // 注册静态资源路由
    register_auth_module(); // 注册认证相关 HTTP 路由
    register_user_module(); // 注册用户相关 HTTP 路由
    register_file_module(); // 注册文件相关 HTTP 路由
}

void CloudDiskServer::register_www_module()
{
    server_.Static("/", "./www/index.html");   // 首页静态文件
    server_.Static("/static", "./www/static"); // 前端 JS/CSS/登录注册页面等静态资源
}

void CloudDiskServer::register_auth_module()
{
    // 用户注册
    server_.POST("/api/v1/auth/register", [this](const HttpReq* req, HttpResp* resp) {

        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        // 读取前端传来的字段
        string username = json_string(body, "username");
        string password = json_string(body, "password");
        string confirm  = json_string(body, "confirm");

        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空");
            return;
        }

        if (password != confirm) {
            response_error(resp, HttpStatusBadRequest, "两次密码不一致");
            return;
        }

        // 创建 Register RPC task
        srpc::SRPCClientTask* task = auth_client_.create_Register_task(
            [resp](pb::RegisterResponse* rpc_resp, srpc::RPCContext* ctx) {

                // 先检查 RPC 通信是否成功
                if (!check_rpc_context(resp, ctx)) return;

                int code = rpc_resp->result().code(); // AuthService 返回的业务 code

                if (code != 0) {
                    // 注册失败时直接把业务错误转成 HTTP JSON
                    response_error(resp,
                                   rpc_code_to_http_status(rpc_resp->result().code()),
                                   rpc_resp->result().message());
                    return;
                }

                // 注册成功时组装前端需要的 data
                json data;
                data["userId"] = rpc_resp->user_id();
                data["username"] = rpc_resp->username();

                response_success(resp, HttpStatusCreated, "注册成功", data);
            });

        // 构造 RPC 请求
        pb::RegisterRequest rpc_req;
        rpc_req.set_username(username);
        rpc_req.set_password(password);

        // 写入 task
        task->serialize_input(&rpc_req);
        // 把 srpc task 接入当前 HTTP 请求序列，避免 HTTP 提前返回空响应
        resp->add_task(task);
    });

    // 用户登陆
    server_.POST("/api/v1/auth/login", [this](const HttpReq* req, HttpResp* resp) {

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

        // 创建 Login RPC task
        srpc::SRPCClientTask* task = auth_client_.create_Login_task(
            [resp](pb::LoginResponse* rpc_resp, srpc::RPCContext* ctx) {

                if (!check_rpc_context(resp, ctx)) return; // 检查通信层

                int code = rpc_resp->result().code(); // 读取业务结果

                if (code != 0) {
                    // 登录失败时返回 AuthService 给出的错误
                    response_error(resp,
                                   rpc_code_to_http_status(rpc_resp->result().code()),
                                   rpc_resp->result().message());
                    return;
                }

                json data;
                data["accessToken"] = rpc_resp->access_token();
                data["tokenType"]   = rpc_resp->token_type();

                data["user"]["userId"]   = rpc_resp->user().user_id();
                data["user"]["username"] = rpc_resp->user().username();

                response_success(resp, HttpStatusOK, "登录成功", data);
            });

        // 构造 LoginRequest
        pb::LoginRequest rpc_req;
        rpc_req.set_username(username);
        rpc_req.set_password(password);

        // 序列化请求
        task->serialize_input(&rpc_req);
        // 把 srpc task 接入当前 HTTP 请求序列，避免 HTTP 提前返回空响应
        resp->add_task(task);
    });
}

void CloudDiskServer::register_user_module()
{
    // 获取当前用户信息
    server_.GET("/api/v1/user/me", [this](const HttpReq* req, HttpResp* resp) {

        // 先通过 AuthService 校验 token
        verify_request_async(req, resp, auth_client_,
            [this, resp](pb::UserIdentity identity) {

                // token 有效后，再调用 UserService 查询用户资料
                srpc::SRPCClientTask* task = user_client_.create_GetUserProfile_task(
                    [resp](pb::GetUserProfileResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!check_rpc_context(resp, ctx)) return; // 检查 RPC 通信层

                        int code = rpc_resp->result().code();
                        if (code != 0) { // 检查业务结果
                            response_error(resp,
                                           rpc_code_to_http_status(rpc_resp->result().code()),
                                           rpc_resp->result().message());
                            return;
                        }

                        json data; // 组装用户资料 JSON
                        data["userId"]   = rpc_resp->user().user_id();
                        data["username"] = rpc_resp->user().username();
                        data["createdAt"] = rpc_resp->user().created_at();

                        response_success(resp, HttpStatusOK, "获取用户信息成功", data);
                    });

                // 构造 GetUserProfileRequest
                pb::GetUserProfileRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());

                // 序列化 RPC 请求
                task->serialize_input(&rpc_req);
                // 把后续 RPC task 继续追加到当前 HTTP 请求序列
                resp->add_task(task);
            });
    });
}

void CloudDiskServer::register_file_module()
{
    // 文件列表查询
    server_.GET("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {

        // 先通过 AuthService 校验 token
        verify_request_async(req, resp, auth_client_,
            [this, resp](pb::UserIdentity identity) {

                // 创建 ListFiles RPC task
                srpc::SRPCClientTask* task = filemeta_client_.create_ListFiles_task(
                    [resp](pb::ListFilesResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!check_rpc_context(resp, ctx)) return; //  检查 RPC 通信层

                        int code = rpc_resp->result().code();
                        if (code != 0) { // 检查业务结果
                            response_error(resp,
                                           rpc_code_to_http_status(rpc_resp->result().code()),
                                           rpc_resp->result().message());
                            return;
                        }

                        // 把 protobuf repeated files 转成前端需要的 JSON 数组
                        json files = json::array();

                        for (const auto& f : rpc_resp->files()) {
                            json item;
                            item["fileId"] = f.file_id();
                            item["filename"] = f.filename();
                            item["size"] = f.size();
                            item["createdAt"] = f.created_at();
                            item["updatedAt"] = f.updated_at();
                            files.push_back(item);
                        }

                        json data;
                        data["files"] = files; // 放进 data.files

                        response_success(resp, HttpStatusOK, "获取文件列表成功", data);
                    });

                // 构造 ListFilesRequest
                pb::ListFilesRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());

                task->serialize_input(&rpc_req); // 序列化 RPC 请求
                resp->add_task(task); // 把 srpc task 加入当前 HTTP 请求序列
            });
    });

    // 上传文件
    // 上传链路：HTTP 接收文件，FileMetaService 写元数据，RabbitMQ 交给 worker 上传 OSS。
    server_.POST("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {
        /*
            上传接口先把 token 从 HTTP 头中拷贝出来。
            后面的 RPC 回调是异步执行的，不能在回调里继续依赖 HttpReq 生命周期。
        */
        string token;
        if (!get_bearer_token(req, token)) {
            response_error(resp, HttpStatusUnauthorized, "无效token");
            return;
        }

        if (req->content_type() != MULTIPART_FORM_DATA) { // 检查上传请求格式
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        Form& form = req->form(); // 读取 multipart/form-data 解析结果

        if (!form.count("file")) { // 前端固定使用字段名 file
            response_error(resp, HttpStatusBadRequest, "缺少文件");
            return;
        }

        /*
            把 filename/content 拷贝成普通 string。
            这样异步回调中不会再依赖 req/form 的生命周期。
        */
        string filename = form["file"].first;
        string content   = form["file"].second;

        if (filename.empty()) { // 文件名不能为空
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            计算文件内容 hash。
            hashcode 会保存到 tbl_file，并作为 OSS 对象名的一部分。
        */
        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());

        // token 校验成功后才能知道 user_id，因此保存临时文件放在回调里做
        verify_token_async(token, resp, auth_client_,
            [this, resp, filename, content = move(content), hashcode]
            (pb::UserIdentity identity) {

                string temp_path; // 保存临时文件路径

                /*
                    把上传内容写入本地临时目录。
                    RabbitMQ 消息只传 tempPath，不直接传文件内容。
                */
                if (!oss_uploader_.save_temp_file(identity.user_id(),
                                                  hashcode,
                                                  content,
                                                  temp_path)) {
                    response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                    return;
                }

                // 创建文件元数据 RPC task
                srpc::SRPCClientTask* task = filemeta_client_.create_CreateFile_task(
                    [this, resp, temp_path, uid = identity.user_id(), filename, hashcode, file_size = content.size()]
                    (pb::CreateFileResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!ctx->success()) { // 如果 RPC 通信失败，删除刚才保存的临时文件
                            cerr << "[RPC FAILED] error=" << ctx->get_error()
                                 << ", msg=" << ctx->get_errmsg()
                                 << endl;
                            oss_uploader_.remove_temp_file(temp_path);
                            response_error(resp, HttpStatusInternalServerError, "RPC失败");
                            return;
                        }

                        if (rpc_resp->result().code() != 0) {
                            // FileMetaService 返回业务错误时，也删除临时文件
                            oss_uploader_.remove_temp_file(temp_path);
                            response_error(resp, HttpStatusInternalServerError, "业务失败");
                            return;
                        }

                        /*
                            元数据写入成功后，网关发布 RabbitMQ 上传任务。
                            后台 oss_upload_worker 会消费这条任务并上传 OSS。
                        */
                        if (!oss_uploader_.publish(uid, hashcode, temp_path)) {
                            oss_uploader_.remove_temp_file(temp_path);
                            response_error(resp, HttpStatusInternalServerError, "MQ失败");
                            return;
                        }

                        json data; // 组装上传成功响应
                        data["fileId"] = rpc_resp->file_id();
                        data["filename"] = rpc_resp->filename();
                        data["size"] = file_size;

                        response_success(resp, HttpStatusCreated, "上传成功", data);
                    });

                // 构造 CreateFileRequest
                pb::CreateFileRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());
                rpc_req.set_filename(filename);
                rpc_req.set_hashcode(hashcode);
                rpc_req.set_size(content.size());

                task->serialize_input(&rpc_req); // 序列化 CreateFile RPC 请求
                resp->add_task(task); // 将 task 加入当前 HTTP 请求的 workflow 序列中
            });
    });

    // 下载文件
    server_.GET("/api/v1/file/{id}", [this](const HttpReq* req, HttpResp* resp) {

        int file_id = req->param<int>("id"); // 先取出路径参数 file_id

        // 校验 token
        verify_request_async(req, resp, auth_client_,
            [this, resp, file_id](pb::UserIdentity identity) {

                // 创建下载元数据查询 RPC task
                srpc::SRPCClientTask* task = filemeta_client_.create_GetFileForDownload_task(
                    [this, resp, uid = identity.user_id()](pb::GetFileForDownloadResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!check_rpc_context(resp, ctx)) return; // 检查 RPC 通信层

                        int code = rpc_resp->result().code();
                        if (code != 0) { // 检查业务结果
                            response_error(resp,
                                           rpc_code_to_http_status(rpc_resp->result().code()),
                                           rpc_resp->result().message());
                            return;
                        }

                        // 取出 FileMetaService 返回的 filename/hashcode
                        string filename = rpc_resp->filename();
                        string hashcode = rpc_resp->hashcode();

                        /*
                            网关仍然负责从 OSS 下载文件字节。
                            这是因为浏览器下载需要本次 HTTP 响应直接返回文件内容。
                        */
                        OssStorage oss;
                        string content;

                        auto status = oss.download_object(uid, hashcode, content);

                        // OSS 中对象不存在时返回 404
                        if (status == OssDownloadStatus::NotFound) {
                            response_error(resp, HttpStatusNotFound, "文件不存在");
                            return;
                        }

                        // OSS 访问失败时返回 500
                        if (status == OssDownloadStatus::Failed) {

                            response_error(resp, HttpStatusInternalServerError, "OSS失败");
                            return;
                        }

                        // 设置 HTTP状态码为 200
                        resp->set_status(HttpStatusOK);
                        // application/octet-stream 表示通用二进制文件
                        resp->add_header("Content-Type", "application/octet-stream");
                        // Content-Disposition 告诉浏览器这是附件下载，并带上文件名
                        resp->add_header("Content-Disposition",
                                         "attachment; filename=\"" +
                                         content_disposition_fallback_filename(filename) +
                                         "\"; filename*=" +
                                         encode_rfc5987_filename(filename));

                        resp->String(move(content)); // 把文件内容写入 HTTP 响应体
                    });

                // 构造下载元数据查询请求
                pb::GetFileForDownloadRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());
                rpc_req.set_file_id(file_id);

                task->serialize_input(&rpc_req); // 序列化 RPC 请求
                resp->add_task(task); // 把 srpc task 加入当前 HTTP 请求序列
            });
    });
}
