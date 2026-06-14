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

/*
    读取字符串环境变量。

    第四期暂时不引入配置中心，服务地址先从环境变量读取；
    如果没有配置，就使用本地默认值。
*/
static string get_env_or_default(const char* name, const string& default_value)
{
    /*
        getenv() 返回环境变量字符串。
        变量不存在时返回 nullptr。
    */
    const char* value = getenv(name);

    /*
        未配置或配置为空时使用默认值。
    */
    if (value == nullptr || string(value).empty()) {
        return default_value;
    }

    /*
        返回环境变量中的实际值。
    */
    return string(value);
}

/*
    读取端口环境变量。

    参数：
    - name：环境变量名。
    - default_port：默认端口。
*/
static unsigned short get_env_port(const char* name, unsigned short default_port)
{
    /*
        先读取环境变量字符串。
    */
    const char* value = getenv(name);

    /*
        没有配置就使用默认端口。
    */
    if (value == nullptr || string(value).empty()) {
        return default_port;
    }

    /*
        把字符串转成长整数。
    */
    char* end = nullptr;
    long port = strtol(value, &end, 10);

    /*
        如果字符串不是纯数字，或者端口超出合法范围，就回退到默认端口。
    */
    if (*end != '\0' || port <= 0 || port > 65535) {
        return default_port;
    }

    /*
        范围已经检查过，可以安全转换。
    */
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
    /*
        设置 HTTP 状态码。
    */
    resp->set_status(status_code);

    /*
        告诉浏览器响应体是 JSON。
    */
    resp->add_header("Content-Type", "application/json");

    /*
        nlohmann::json::dump() 把 JSON 对象序列化成字符串。
    */
    resp->String(body.dump());
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
    /*
        创建 JSON 响应体。
    */
    json body;

    /*
        status 固定为 success。
    */
    body["status"] = "success";

    /*
        message 是给前端展示的提示文本。
    */
    body["message"] = message;

    /*
        data 保存真正的业务数据。
    */
    body["data"] = data;

    /*
        交给统一 JSON 响应函数写回 HTTP。
    */
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
    /*
        创建 JSON 响应体。
    */
    json body;

    /*
        status 固定为 error。
    */
    body["status"] = "error";

    /*
        message 保存错误原因。
    */
    body["message"] = message;

    /*
        写回 HTTP。
    */
    response_json(resp, status_code, body);
}

/*
    将 RPC 业务错误码转换成 HTTP 状态码。

    当前 proto 中约定业务错误码直接使用 400/401/404/409/500，
    所以大部分情况直接返回 code 即可。
*/
static int rpc_code_to_http_status(int code)
{
    /*
        只接受常见 HTTP 错误码。
        其它未知 code 统一当作 500，避免返回奇怪状态码。
    */
    if (code == 400 || code == 401 || code == 404 || code == 409 || code == 500) {
        return code;
    }

    /*
        未知错误按内部服务器错误处理。
    */
    return HttpStatusInternalServerError;
}

/*
    处理 RPC 通信层失败。

    ctx->success() == false 说明不是业务错误，而是 RPC 网络/序列化/服务不可达等问题。
*/
static bool check_rpc_context(HttpResp* resp, srpc::RPCContext* ctx)
{
    /*
        RPC 成功到达服务端并拿到响应时，success() 为 true。
    */
    if (ctx->success()) {
        return true;
    }

    /*
        这里把底层错误打印到服务端日志，方便排查后端服务是否启动。
    */
    cerr << "[RPC FAILED] error=" << ctx->get_error()
         << ", msg=" << ctx->get_errmsg()
         << endl;

    /*
        对前端统一返回内部服务器错误，不暴露内部地址和 RPC 细节。
    */
    response_error(resp, HttpStatusInternalServerError, "内部服务器错误");

    /*
        false 表示调用方不能继续读取业务响应。
    */
    return false;
}

/*
    解析 JSON 请求体。
*/
static bool parse_json_body(const HttpReq* req, json& body)
{
    /*
        注册和登录都要求 application/json。
    */
    if (req->content_type() != APPLICATION_JSON) {
        return false;
    }

    /*
        parse(..., false) 表示解析失败时不抛异常，而是返回 discarded。
    */
    body = json::parse(req->body(), nullptr, false);

    /*
        不是 discarded 就说明 JSON 语法合法。
    */
    return !body.is_discarded();
}

/*
    从 JSON 中读取字符串字段。
*/
static string json_string(const json& body, const string& key)
{
    /*
        字段不存在或类型不是字符串时，返回空字符串。
    */
    if (!body.contains(key) || !body[key].is_string()) {
        return "";
    }

    /*
        字段存在且是字符串，取出它。
    */
    return body[key].get<string>();
}

/*
    从 Authorization 头中取 Bearer Token。
*/
static bool get_bearer_token(const HttpReq* req, string& token)
{
    /*
        没有 Authorization 头，说明没有携带登录态。
    */
    if (!req->has_header("Authorization")) {
        return false;
    }

    /*
        读取完整 Authorization 头。
    */
    const string& authorization = req->header("Authorization");

    /*
        本项目只接受 Bearer Token。
    */
    const string prefix = "Bearer ";

    /*
        长度不够时，不可能包含有效 token。
    */
    if (authorization.size() <= prefix.size()) {
        return false;
    }

    /*
        前缀必须严格等于 "Bearer "。
    */
    if (authorization.substr(0, prefix.size()) != prefix) {
        return false;
    }

    /*
        截掉 Bearer 前缀，得到真正的 token。
    */
    token = authorization.substr(prefix.size());

    /*
        token 不能为空。
    */
    return !token.empty();
}

/*
    生成 Content-Disposition 中 filename= 使用的 ASCII 兜底文件名。
*/
static string content_disposition_fallback_filename(const string& filename)
{
    /*
        result 保存最终的 ASCII 文件名。
    */
    string result;

    /*
        按字节扫描文件名。
        中文 UTF-8 字节会被替换成 '_'，避免老浏览器乱码。
    */
    for (unsigned char ch : filename) {
        /*
            双引号和反斜线在 HTTP header 参数中需要转义。
        */
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += ch;
        /*
            可打印 ASCII 字符可以直接放进 filename=。
        */
        } else if (ch >= 0x20 && ch <= 0x7e) {
            result += ch;
        /*
            非 ASCII 字节统一替换成下划线。
        */
        } else {
            result += '_';
        }
    }

    /*
        如果最终为空，给一个默认文件名。
    */
    if (result.empty()) {
        return "download";
    }

    /*
        返回兜底文件名。
    */
    return result;
}

/*
    判断字符是否可以直接出现在 RFC 5987 filename* 参数中。
*/
static bool is_rfc5987_attr_char(unsigned char ch)
{
    /*
        数字可以直接出现。
    */
    if (ch >= '0' && ch <= '9') {
        return true;
    }

    /*
        大写字母可以直接出现。
    */
    if (ch >= 'A' && ch <= 'Z') {
        return true;
    }

    /*
        小写字母可以直接出现。
    */
    if (ch >= 'a' && ch <= 'z') {
        return true;
    }

    /*
        RFC 允许的少量符号。
    */
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

/*
    把 UTF-8 文件名编码成 filename* 可用的格式。
*/
static string encode_rfc5987_filename(const string& filename)
{
    /*
        十六进制字符表，用于百分号编码。
    */
    static const char* hex = "0123456789ABCDEF";

    /*
        filename* 需要带上字符集前缀。
    */
    string result = "UTF-8''";

    /*
        按 UTF-8 字节逐个处理。
    */
    for (unsigned char ch : filename) {
        /*
            允许直接出现的字符不编码。
        */
        if (is_rfc5987_attr_char(ch)) {
            result += ch;
        /*
            其它字节写成 %XX。
        */
        } else {
            result += '%';
            result += hex[ch >> 4];
            result += hex[ch & 0x0f];
        }
    }

    /*
        返回编码后的 filename* 参数值。
    */
    return result;
}

/*
    根据 token 字符串异步调用 AuthService.VerifyToken。

    参数：
    - token：已经从 HTTP Header 中解析出来的 Bearer Token。
    - resp：当前 HTTP 响应对象。
    - auth_client：AuthService srpc 客户端。
    - next：token 校验成功后要继续执行的业务逻辑。
*/
static void verify_token_async(const string& token,
                               HttpResp* resp,
                               pb::AuthService::SRPCClient& auth_client,
                               const function<void(pb::UserIdentity)>& next)
{
    /*
        创建 VerifyToken RPC task。
        这个 task 启动后会异步访问 AuthService。
    */
    srpc::SRPCClientTask* task = auth_client.create_VerifyToken_task(
        [resp, next](pb::VerifyTokenResponse* rpc_resp, srpc::RPCContext* ctx) {
            /*
                先判断 RPC 通信层是否成功。
            */
            if (!check_rpc_context(resp, ctx)) {
                return;
            }

            /*
                读取业务结果码。
            */
            int code = rpc_resp->result().code();

            /*
                code != 0 表示 token 无效或服务端业务错误。
            */
            if (code != 0) {
                response_error(resp,
                               rpc_code_to_http_status(code),
                               rpc_resp->result().message());
                return;
            }

            /*
                token 校验成功，把用户身份传给下一步业务。
                这里按值传递，避免 rpc_resp 回调结束后对象失效。
            */
            next(rpc_resp->user());
        });

    /*
        构造 protobuf 请求。
    */
    pb::VerifyTokenRequest rpc_req;

    /*
        写入 token 字符串。
    */
    rpc_req.set_access_token(token);

    /*
        serialize_input 会把请求序列化进 srpc task。
    */
    task->serialize_input(&rpc_req);

    /*
        关键点：这里不能直接 task->start()。

        wfrest 的 HTTP 处理函数返回后，如果没有把异步任务接到当前 HTTP 请求的
        SeriesWork 上，框架会认为响应已经结束，浏览器就会收到空响应。

        resp->add_task(task) 会把 srpc task 加入当前 HTTP 响应所在的 workflow 序列，
        等 RPC 回调写完 resp 后，HTTP 响应才真正结束。
    */
    resp->add_task(task);
}

/*
    从 HTTP 请求头读取 token，并调用 verify_token_async。
*/
static void verify_request_async(const HttpReq* req,
                                 HttpResp* resp,
                                 pb::AuthService::SRPCClient& auth_client,
                                 const function<void(pb::UserIdentity)>& next)
{
    /*
        token 用来保存解析出的 Bearer Token。
    */
    string token;

    /*
        请求没有携带合法 Bearer Token 时，直接返回 401。
    */
    if (!get_bearer_token(req, token)) {
        response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
        return;
    }

    /*
        token 字符串已经拷贝出来，后续异步回调不再依赖 HttpReq 生命周期。
    */
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

CloudDiskServer::~CloudDiskServer()
{
    /*
        网关没有启动 RabbitMQ 消费线程，所以析构时不需要 stop()。
        RabbitMqOssUploader 析构函数内部也会兜底调用 stop()，即使没启动也安全。
    */
}

void CloudDiskServer::register_routes()
{
    /*
        注册静态资源路由。
    */
    register_www_module();

    /*
        注册认证相关 HTTP 路由。
    */
    register_auth_module();

    /*
        注册用户相关 HTTP 路由。
    */
    register_user_module();

    /*
        注册文件相关 HTTP 路由。
    */
    register_file_module();
}

void CloudDiskServer::register_www_module()
{
    /*
        首页静态文件。
    */
    server_.Static("/", "./www/index.html");

    /*
        前端 JS/CSS/登录注册页面等静态资源。
    */
    server_.Static("/static", "./www/static");
}

void CloudDiskServer::register_auth_module()
{
    /*
        POST /api/v1/auth/register
    */
    server_.POST("/api/v1/auth/register", [this](const HttpReq* req, HttpResp* resp) {
        /*
            解析 JSON 请求体。
        */
        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            读取前端传来的字段。
        */
        string username = json_string(body, "username");
        string password = json_string(body, "password");
        string confirm = json_string(body, "confirm");

        /*
            网关先做 HTTP 表单级校验。
        */
        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空");
            return;
        }

        /*
            confirm 不传给 AuthService，只在网关判断两次密码是否一致。
        */
        if (password != confirm) {
            response_error(resp, HttpStatusBadRequest, "两次输入的密码不一致");
            return;
        }

        /*
            创建 Register RPC task。
        */
        srpc::SRPCClientTask* task = auth_client_.create_Register_task(
            [resp](pb::RegisterResponse* rpc_resp, srpc::RPCContext* ctx) {
                /*
                    先检查 RPC 通信是否成功。
                */
                if (!check_rpc_context(resp, ctx)) {
                    return;
                }

                /*
                    AuthService 返回的业务 code。
                */
                int code = rpc_resp->result().code();

                /*
                    注册失败时直接把业务错误转成 HTTP JSON。
                */
                if (code != 0) {
                    response_error(resp,
                                   rpc_code_to_http_status(code),
                                   rpc_resp->result().message());
                    return;
                }

                /*
                    注册成功时组装前端需要的 data。
                */
                json data;
                data["userId"] = rpc_resp->user_id();
                data["username"] = rpc_resp->username();

                /*
                    注册成功仍然返回 201。
                */
                response_success(resp, HttpStatusCreated, "注册成功", data);
            });

        /*
            构造 RPC 请求。
        */
        pb::RegisterRequest rpc_req;
        rpc_req.set_username(username);
        rpc_req.set_password(password);

        /*
            写入 task。
        */
        task->serialize_input(&rpc_req);

        /*
            把 srpc task 接入当前 HTTP 请求序列，避免 HTTP 提前返回空响应。
        */
        resp->add_task(task);
    });

    /*
        POST /api/v1/auth/login
    */
    server_.POST("/api/v1/auth/login", [this](const HttpReq* req, HttpResp* resp) {
        /*
            解析 JSON。
        */
        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            读取用户名和密码。
        */
        string username = json_string(body, "username");
        string password = json_string(body, "password");

        /*
            基础参数校验。
        */
        if (username.empty() || password.empty()) {
            response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空");
            return;
        }

        /*
            创建 Login RPC task。
        */
        srpc::SRPCClientTask* task = auth_client_.create_Login_task(
            [resp](pb::LoginResponse* rpc_resp, srpc::RPCContext* ctx) {
                /*
                    检查通信层。
                */
                if (!check_rpc_context(resp, ctx)) {
                    return;
                }

                /*
                    读取业务结果。
                */
                int code = rpc_resp->result().code();

                /*
                    登录失败时返回 AuthService 给出的错误。
                */
                if (code != 0) {
                    response_error(resp,
                                   rpc_code_to_http_status(code),
                                   rpc_resp->result().message());
                    return;
                }

                /*
                    登录成功时，按第三期前端需要的 JSON 结构返回。
                */
                json data;
                data["accessToken"] = rpc_resp->access_token();
                data["tokenType"] = rpc_resp->token_type();
                data["user"]["userId"] = rpc_resp->user().user_id();
                data["user"]["username"] = rpc_resp->user().username();

                /*
                    返回 200。
                */
                response_success(resp, HttpStatusOK, "登录成功", data);
            });

        /*
            构造 LoginRequest。
        */
        pb::LoginRequest rpc_req;
        rpc_req.set_username(username);
        rpc_req.set_password(password);

        /*
            序列化请求。
        */
        task->serialize_input(&rpc_req);

        /*
            把 srpc task 接入当前 HTTP 请求序列，避免 HTTP 提前返回空响应。
        */
        resp->add_task(task);
    });
}

void CloudDiskServer::register_user_module()
{
    /*
        GET /api/v1/user/me
    */
    server_.GET("/api/v1/user/me", [this](const HttpReq* req, HttpResp* resp) {
        /*
            先通过 AuthService 校验 token。
        */
        verify_request_async(req, resp, auth_client_, [this, resp](pb::UserIdentity identity) {
            /*
                token 有效后，再调用 UserService 查询用户资料。
            */
            srpc::SRPCClientTask* task = user_client_.create_GetUserProfile_task(
                [resp](pb::GetUserProfileResponse* rpc_resp, srpc::RPCContext* ctx) {
                    /*
                        检查 RPC 通信层。
                    */
                    if (!check_rpc_context(resp, ctx)) {
                        return;
                    }

                    /*
                        检查业务结果。
                    */
                    int code = rpc_resp->result().code();
                    if (code != 0) {
                        response_error(resp,
                                       rpc_code_to_http_status(code),
                                       rpc_resp->result().message());
                        return;
                    }

                    /*
                        组装用户资料 JSON。
                    */
                    json data;
                    data["userId"] = rpc_resp->user().user_id();
                    data["username"] = rpc_resp->user().username();
                    data["createdAt"] = rpc_resp->user().created_at();

                    /*
                        返回成功响应。
                    */
                    response_success(resp, HttpStatusOK, "获取个人信息成功", data);
                });

            /*
                构造 GetUserProfileRequest。
            */
            pb::GetUserProfileRequest rpc_req;
            rpc_req.set_user_id(identity.user_id());

            /*
                序列化 RPC 请求。
            */
            task->serialize_input(&rpc_req);

            /*
                把后续 RPC task 继续追加到当前 HTTP 请求序列。
            */
            resp->add_task(task);
        });
    });
}

void CloudDiskServer::register_file_module()
{
    /*
        GET /api/v1/files
    */
    server_.GET("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {
        /*
            文件列表需要先校验 token。
        */
        verify_request_async(req, resp, auth_client_, [this, resp](pb::UserIdentity identity) {
            /*
                创建 ListFiles RPC task。
            */
            srpc::SRPCClientTask* task = filemeta_client_.create_ListFiles_task(
                [resp](pb::ListFilesResponse* rpc_resp, srpc::RPCContext* ctx) {
                    /*
                        检查 RPC 通信层。
                    */
                    if (!check_rpc_context(resp, ctx)) {
                        return;
                    }

                    /*
                        检查业务结果。
                    */
                    int code = rpc_resp->result().code();
                    if (code != 0) {
                        response_error(resp,
                                       rpc_code_to_http_status(code),
                                       rpc_resp->result().message());
                        return;
                    }

                    /*
                        把 protobuf repeated files 转成前端需要的 JSON 数组。
                    */
                    json files = json::array();
                    for (const pb::FileInfo& rpc_file : rpc_resp->files()) {
                        json file;
                        file["fileId"] = rpc_file.file_id();
                        file["filename"] = rpc_file.filename();
                        file["size"] = rpc_file.size();
                        file["fileSize"] = rpc_file.size();
                        file["file_size"] = rpc_file.size();
                        file["createdAt"] = rpc_file.created_at();
                        file["updatedAt"] = rpc_file.updated_at();
                        files.push_back(file);
                    }

                    /*
                        放进 data.files。
                    */
                    json data;
                    data["files"] = files;

                    /*
                        返回成功响应。
                    */
                    response_success(resp, HttpStatusOK, "获取文件列表成功", data);
                });

            /*
                构造 ListFilesRequest。
            */
            pb::ListFilesRequest rpc_req;
            rpc_req.set_user_id(identity.user_id());

            /*
                序列化 RPC 请求。
            */
            task->serialize_input(&rpc_req);

            /*
                把 srpc task 接入当前 HTTP 请求序列。
            */
            resp->add_task(task);
        });
    });

    /*
        POST /api/v1/files
    */
    server_.POST("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {
        /*
            上传接口先把 token 从 HTTP 头中拷贝出来。
            后面的 RPC 回调是异步执行的，不能在回调里继续依赖 HttpReq 生命周期。
        */
        string token;
        if (!get_bearer_token(req, token)) {
            response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
            return;
        }

        /*
            检查上传请求格式。
        */
        if (req->content_type() != MULTIPART_FORM_DATA) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            读取 multipart/form-data 解析结果。
        */
        Form& form = req->form();

        /*
            前端固定使用字段名 file。
        */
        if (!form.count("file")) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            把 filename/content 拷贝成普通 string。
            这样异步回调中不会再依赖 req/form 的生命周期。
        */
        string filename = form["file"].first;
        string content = form["file"].second;

        /*
            文件名不能为空。
        */
        if (filename.empty()) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        /*
            计算文件内容 hash。
            hashcode 会保存到 tbl_file，并作为 OSS 对象名的一部分。
        */
        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());

        /*
            token 校验成功后才能知道 user_id，因此保存临时文件放在回调里做。
        */
        verify_token_async(token,
                           resp,
                           auth_client_,
                           [this, resp, filename, content = move(content), hashcode](pb::UserIdentity identity) {
            /*
                保存临时文件路径。
            */
            string temp_path;

            /*
                把上传内容写入本地临时目录。
                RabbitMQ 消息只传 tempPath，不直接传文件内容。
            */
            if (!oss_uploader_.save_temp_file(identity.user_id(), hashcode, content, temp_path)) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            /*
                创建文件元数据 RPC task。
            */
            srpc::SRPCClientTask* task = filemeta_client_.create_CreateFile_task(
                [this, resp, uid = identity.user_id(), filename, hashcode, temp_path, file_size = content.size()](
                    pb::CreateFileResponse* rpc_resp,
                    srpc::RPCContext* ctx) {
                    /*
                        如果 RPC 通信失败，删除刚才保存的临时文件。
                    */
                    if (!ctx->success()) {
                        cerr << "[RPC FAILED] error=" << ctx->get_error()
                             << ", msg=" << ctx->get_errmsg()
                             << endl;
                        oss_uploader_.remove_temp_file(temp_path);
                        response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                        return;
                    }

                    /*
                        FileMetaService 返回业务错误时，也删除临时文件。
                    */
                    int code = rpc_resp->result().code();
                    if (code != 0) {
                        oss_uploader_.remove_temp_file(temp_path);
                        response_error(resp,
                                       rpc_code_to_http_status(code),
                                       rpc_resp->result().message());
                        return;
                    }

                    /*
                        元数据写入成功后，网关发布 RabbitMQ 上传任务。
                        后台 oss_upload_worker 会消费这条任务并上传 OSS。
                    */
                    if (!oss_uploader_.publish(uid, hashcode, temp_path)) {
                        oss_uploader_.remove_temp_file(temp_path);
                        response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                        return;
                    }

                    /*
                        组装上传成功响应。
                    */
                    json data;
                    data["fileId"] = rpc_resp->file_id();
                    data["filename"] = rpc_resp->filename();
                    data["size"] = file_size;
                    data["fileSize"] = file_size;
                    data["file_size"] = file_size;

                    /*
                        返回 201。
                    */
                    response_success(resp, HttpStatusCreated, "上传成功", data);
                });

            /*
                构造 CreateFileRequest。
            */
            pb::CreateFileRequest rpc_req;
            rpc_req.set_user_id(identity.user_id());
            rpc_req.set_filename(filename);
            rpc_req.set_hashcode(hashcode);
            rpc_req.set_size(static_cast<long long>(content.size()));

            /*
                序列化 CreateFile RPC 请求。
            */
            task->serialize_input(&rpc_req);

            /*
                这里同样不能直接 task->start()。
                这是 VerifyToken 回调中追加的第二个 RPC task，
                仍然要放回当前 HTTP 请求的 workflow 序列中。
            */
            resp->add_task(task);
        });
    });

    /*
        GET /api/v1/file/{id}
    */
    server_.GET("/api/v1/file/{id}", [this](const HttpReq* req, HttpResp* resp) {
        /*
            先取出路径参数 file_id。
            这个值要拷贝出来，异步回调中不再访问 req。
        */
        int file_id = req->param<int>("id");

        /*
            校验 token。
        */
        verify_request_async(req, resp, auth_client_, [this, resp, file_id](pb::UserIdentity identity) {
            /*
                创建下载元数据查询 RPC task。
            */
            srpc::SRPCClientTask* task = filemeta_client_.create_GetFileForDownload_task(
                [this, resp, uid = identity.user_id()](
                    pb::GetFileForDownloadResponse* rpc_resp,
                    srpc::RPCContext* ctx) {
                    /*
                        检查 RPC 通信层。
                    */
                    if (!check_rpc_context(resp, ctx)) {
                        return;
                    }

                    /*
                        检查业务结果。
                    */
                    int code = rpc_resp->result().code();
                    if (code != 0) {
                        response_error(resp,
                                       rpc_code_to_http_status(code),
                                       rpc_resp->result().message());
                        return;
                    }

                    /*
                        取出 FileMetaService 返回的 filename/hashcode。
                    */
                    string filename = rpc_resp->filename();
                    string hashcode = rpc_resp->hashcode();

                    /*
                        网关仍然负责从 OSS 下载文件字节。
                        这是因为浏览器下载需要本次 HTTP 响应直接返回文件内容。
                    */
                    OssStorage oss_storage;
                    string content;
                    OssDownloadStatus status = oss_storage.download_object(uid, hashcode, content);

                    /*
                        OSS 中对象不存在时返回 404。
                    */
                    if (status == OssDownloadStatus::NotFound) {
                        response_error(resp, HttpStatusNotFound, "文件不存在");
                        return;
                    }

                    /*
                        OSS 访问失败时返回 500。
                    */
                    if (status == OssDownloadStatus::Failed) {
                        response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                        return;
                    }

                    /*
                        设置 HTTP 200。
                    */
                    resp->set_status(HttpStatusOK);

                    /*
                        application/octet-stream 表示通用二进制文件。
                    */
                    resp->add_header("Content-Type", "application/octet-stream");

                    /*
                        Content-Disposition 告诉浏览器这是附件下载，并带上文件名。
                    */
                    resp->add_header("Content-Disposition",
                                     "attachment; filename=\"" +
                                         content_disposition_fallback_filename(filename) +
                                         "\"; filename*=" +
                                         encode_rfc5987_filename(filename));

                    /*
                        把文件内容写入 HTTP 响应体。
                    */
                    resp->String(move(content));
                });

            /*
                构造下载元数据查询请求。
            */
            pb::GetFileForDownloadRequest rpc_req;
            rpc_req.set_user_id(identity.user_id());
            rpc_req.set_file_id(file_id);

            /*
                序列化 RPC 请求。
            */
            task->serialize_input(&rpc_req);

            /*
                把 srpc task 接入当前 HTTP 请求序列。
            */
            resp->add_task(task);
        });
    });
}
