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

namespace pb = cloud::disk;

static string get_env_or_default(const char* name, const string& default_value)
{
    const char* value = getenv(name);

    if (!value || string(value).empty()) {
        return default_value;
    }

    return string(value);
}

static unsigned short get_env_port(const char* name, unsigned short default_port)
{
    const char* value = getenv(name);

    if (!value || string(value).empty()) {
        return default_port;
    }

    char* end = nullptr;
    long port = strtol(value, &end, 10);

    if (*end != '\0' || port <= 0 || port > 65535) {
        return default_port;
    }

    return static_cast<unsigned short>(port);
}

static const string AuthServiceHost = get_env_or_default("AUTH_SERVICE_HOST", "127.0.0.1");
static const unsigned short AuthServicePort = get_env_port("AUTH_SERVICE_PORT", 9001);

static const string UserServiceHost = get_env_or_default("USER_SERVICE_HOST", "127.0.0.1");
static const unsigned short UserServicePort = get_env_port("USER_SERVICE_PORT", 9002);

static const string FileMetaServiceHost = get_env_or_default("FILEMETA_SERVICE_HOST", "127.0.0.1");
static const unsigned short FileMetaServicePort = get_env_port("FILEMETA_SERVICE_PORT", 9003);

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

static int rpc_code_to_http_status(int code)
{
    if (code == 400 || code == 401 || code == 404 || code == 409 || code == 500) {
        return code;
    }
    return HttpStatusInternalServerError;
}

static bool check_rpc_context(HttpResp* resp, srpc::RPCContext* ctx)
{
    if (ctx->success()) {
        return true;
    }

    cerr << "[RPC FAILED] error=" << ctx->get_error()
         << ", msg=" << ctx->get_errmsg() << endl;

    response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
    return false;
}

static bool parse_json_body(const HttpReq* req, json& body)
{
    if (req->content_type() != APPLICATION_JSON) {
        return false;
    }

    body = json::parse(req->body(), nullptr, false);
    return !body.is_discarded();
}

static string json_string(const json& body, const string& key)
{
    if (!body.contains(key) || !body[key].is_string()) {
        return "";
    }
    return body[key].get<string>();
}

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

    return result.empty() ? "download" : result;
}

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

// 统一的鉴权入口：网关异步调用 AuthService，成功后继续执行业务回调。
static void verify_token_async(
    const string& token,
    HttpResp* resp,
    pb::AuthService::SRPCClient& auth_client,
    const function<void(pb::UserIdentity)>& next)
{
    srpc::SRPCClientTask* task =
        auth_client.create_VerifyToken_task(
            [resp, next](pb::VerifyTokenResponse* rpc_resp, srpc::RPCContext* ctx) {

                if (!check_rpc_context(resp, ctx)) {
                    return;
                }

                if (rpc_resp->result().code() != 0) {
                    response_error(resp,
                                   rpc_code_to_http_status(rpc_resp->result().code()),
                                   rpc_resp->result().message());
                    return;
                }

                next(rpc_resp->user());
            });

    pb::VerifyTokenRequest rpc_req;
    rpc_req.set_access_token(token);
    task->serialize_input(&rpc_req);

    resp->add_task(task);
}

static void verify_request_async(
    const HttpReq* req,
    HttpResp* resp,
    pb::AuthService::SRPCClient& auth_client,
    const function<void(pb::UserIdentity)>& next)
{
    string token;

    if (!get_bearer_token(req, token)) {
        response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
        return;
    }

    verify_token_async(token, resp, auth_client, next);
}

CloudDiskServer::CloudDiskServer()
    : oss_uploader_()
    , auth_client_(AuthServiceHost.c_str(), AuthServicePort)
    , user_client_(UserServiceHost.c_str(), UserServicePort)
    , filemeta_client_(FileMetaServiceHost.c_str(), FileMetaServicePort)
{
}

CloudDiskServer::~CloudDiskServer() = default;

void CloudDiskServer::register_routes()
{
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

void CloudDiskServer::register_auth_module()
{
    server_.POST("/api/v1/auth/register", [this](const HttpReq* req, HttpResp* resp) {

        json body;
        if (!parse_json_body(req, body)) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

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

        auto task = auth_client_.create_Register_task(
            [resp](pb::RegisterResponse* rpc_resp, srpc::RPCContext* ctx) {

                if (!check_rpc_context(resp, ctx)) return;

                if (rpc_resp->result().code() != 0) {
                    response_error(resp,
                                   rpc_code_to_http_status(rpc_resp->result().code()),
                                   rpc_resp->result().message());
                    return;
                }

                json data;
                data["userId"] = rpc_resp->user_id();
                data["username"] = rpc_resp->username();

                response_success(resp, HttpStatusCreated, "注册成功", data);
            });

        pb::RegisterRequest rpc_req;
        rpc_req.set_username(username);
        rpc_req.set_password(password);

        task->serialize_input(&rpc_req);
        resp->add_task(task);
    });

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

        auto task = auth_client_.create_Login_task(
            [resp](pb::LoginResponse* rpc_resp, srpc::RPCContext* ctx) {

                if (!check_rpc_context(resp, ctx)) return;

                if (rpc_resp->result().code() != 0) {
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

        pb::LoginRequest rpc_req;
        rpc_req.set_username(username);
        rpc_req.set_password(password);

        task->serialize_input(&rpc_req);
        resp->add_task(task);
    });
}

void CloudDiskServer::register_user_module()
{
    server_.GET("/api/v1/user/me", [this](const HttpReq* req, HttpResp* resp) {

        verify_request_async(req, resp, auth_client_,
            [this, resp](pb::UserIdentity identity) {

                auto task = user_client_.create_GetUserProfile_task(
                    [resp](pb::GetUserProfileResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!check_rpc_context(resp, ctx)) return;

                        if (rpc_resp->result().code() != 0) {
                            response_error(resp,
                                           rpc_code_to_http_status(rpc_resp->result().code()),
                                           rpc_resp->result().message());
                            return;
                        }

                        json data;
                        data["userId"]   = rpc_resp->user().user_id();
                        data["username"] = rpc_resp->user().username();
                        data["createdAt"] = rpc_resp->user().created_at();

                        response_success(resp, HttpStatusOK, "获取用户信息成功", data);
                    });

                pb::GetUserProfileRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());

                task->serialize_input(&rpc_req);
                resp->add_task(task);
            });
    });
}

void CloudDiskServer::register_file_module()
{
    server_.GET("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {

        verify_request_async(req, resp, auth_client_,
            [this, resp](pb::UserIdentity identity) {

                auto task = filemeta_client_.create_ListFiles_task(
                    [resp](pb::ListFilesResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!check_rpc_context(resp, ctx)) return;

                        if (rpc_resp->result().code() != 0) {
                            response_error(resp,
                                           rpc_code_to_http_status(rpc_resp->result().code()),
                                           rpc_resp->result().message());
                            return;
                        }

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
                        data["files"] = files;

                        response_success(resp, HttpStatusOK, "获取文件列表成功", data);
                    });

                pb::ListFilesRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());

                task->serialize_input(&rpc_req);
                resp->add_task(task);
            });
    });

    // 上传链路：HTTP 接收文件，FileMetaService 写元数据，RabbitMQ 交给 worker 上传 OSS。
    server_.POST("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {

        string token;
        if (!get_bearer_token(req, token)) {
            response_error(resp, HttpStatusUnauthorized, "无效token");
            return;
        }

        if (req->content_type() != MULTIPART_FORM_DATA) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        Form& form = req->form();

        if (!form.count("file")) {
            response_error(resp, HttpStatusBadRequest, "缺少文件");
            return;
        }

        string filename = form["file"].first;
        string content   = form["file"].second;

        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());

        verify_token_async(token, resp, auth_client_,
            [this, resp, filename, content = move(content), hashcode]
            (pb::UserIdentity identity) {

                string temp_path;

                if (!oss_uploader_.save_temp_file(identity.user_id(),
                                                  hashcode,
                                                  content,
                                                  temp_path)) {
                    response_error(resp, HttpStatusInternalServerError, "内部错误");
                    return;
                }

                auto task = filemeta_client_.create_CreateFile_task(
                    [this, resp, temp_path, uid = identity.user_id(), filename, hashcode, file_size = content.size()]
                    (pb::CreateFileResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!ctx->success()) {
                            oss_uploader_.remove_temp_file(temp_path);
                            response_error(resp, HttpStatusInternalServerError, "RPC失败");
                            return;
                        }

                        if (rpc_resp->result().code() != 0) {
                            oss_uploader_.remove_temp_file(temp_path);
                            response_error(resp, HttpStatusInternalServerError, "业务失败");
                            return;
                        }

                        if (!oss_uploader_.publish(uid, hashcode, temp_path)) {
                            oss_uploader_.remove_temp_file(temp_path);
                            response_error(resp, HttpStatusInternalServerError, "MQ失败");
                            return;
                        }

                        json data;
                        data["fileId"] = rpc_resp->file_id();
                        data["filename"] = rpc_resp->filename();
                        data["size"] = file_size;

                        response_success(resp, HttpStatusCreated, "上传成功", data);
                    });

                pb::CreateFileRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());
                rpc_req.set_filename(filename);
                rpc_req.set_hashcode(hashcode);
                rpc_req.set_size(content.size());

                task->serialize_input(&rpc_req);
                resp->add_task(task);
            });
    });

    server_.GET("/api/v1/file/{id}", [this](const HttpReq* req, HttpResp* resp) {

        int file_id = req->param<int>("id");

        verify_request_async(req, resp, auth_client_,
            [this, resp, file_id](pb::UserIdentity identity) {

                auto task = filemeta_client_.create_GetFileForDownload_task(
                    [this, resp, uid = identity.user_id()](pb::GetFileForDownloadResponse* rpc_resp, srpc::RPCContext* ctx) {

                        if (!check_rpc_context(resp, ctx)) return;

                        if (rpc_resp->result().code() != 0) {
                            response_error(resp,
                                           rpc_code_to_http_status(rpc_resp->result().code()),
                                           rpc_resp->result().message());
                            return;
                        }

                        string filename = rpc_resp->filename();
                        string hashcode = rpc_resp->hashcode();

                        OssStorage oss;
                        string content;

                        auto status = oss.download_object(uid, hashcode, content);

                        if (status == OssDownloadStatus::NotFound) {
                            response_error(resp, HttpStatusNotFound, "文件不存在");
                            return;
                        }

                        if (status == OssDownloadStatus::Failed) {
                            response_error(resp, HttpStatusInternalServerError, "OSS失败");
                            return;
                        }

                        resp->set_status(HttpStatusOK);
                        resp->add_header("Content-Type", "application/octet-stream");

                        resp->add_header("Content-Disposition",
                                         "attachment; filename=\"" +
                                         content_disposition_fallback_filename(filename) +
                                         "\"; filename*=" +
                                         encode_rfc5987_filename(filename));

                        resp->String(move(content));
                    });

                pb::GetFileForDownloadRequest rpc_req;
                rpc_req.set_user_id(identity.user_id());
                rpc_req.set_file_id(file_id);

                task->serialize_input(&rpc_req);
                resp->add_task(task);
            });
    });
}
