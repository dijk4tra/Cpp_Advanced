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

    注意这里的 user 是一个“输出参数”：
    - 调用方先定义一个空的局部变量：User user;
    - 然后把它传进来：check_login(req, user);
    - verify_token(token, user) 会把 token 里的 id、username、created_at 写入这个 user

    所以后续代码能访问 user.id、user.username，不是因为登录时的 User 对象还存在，
    而是因为“本次请求”重新从 Authorization 头里的 JWT 解码出了用户信息。
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
        3. 检查用户名/密码是否为空以及两次密码是否一致
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
            /*
                INSERT/UPDATE/DELETE 这类“不返回结果集”的 SQL，成功时状态通常是 MYSQL_STATUS_OK。
                这里执行的是 INSERT INTO tbl_user。

                如果状态不是 MYSQL_STATUS_OK，说明这条插入语句没有被 MySQL 正常执行完成。
                在本接口里最常见的原因是 username 字段有 UNIQUE 约束，用户名重复会导致插入失败。

                PDF 要求“插入记录失败”返回 409 + “用户名已存在”。
                所以这里把 INSERT 失败统一按用户名已存在处理。
            */
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
            /*
                这里执行的是 SELECT 语句。

                对 SELECT 来说，正常情况应该得到一个“结果集”，wfrest/workflow 用
                MYSQL_STATUS_GET_RESULT 表示“SQL 执行成功，并且 cursor 中有可读取的结果集”。

                如果状态不是 MYSQL_STATUS_GET_RESULT，说明不是“查不到用户”这种业务失败，
                而是 SQL 执行过程本身出了问题，例如：
                - 数据库连接失败
                - SQL 语法错误
                - 表名/字段名写错
                - MySQL 服务异常

                这些问题都属于服务器端或数据库端问题，不应该告诉前端“用户名或密码错误”，
                所以按 PDF 要求返回 500 + “内部服务器错误”。
            */
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            /*
                能走到这里，说明 SELECT 语句本身执行成功了。

                get_rows_count() 表示这个 SELECT 查询到了多少行记录。
                登录 SQL 的 WHERE 条件是：
                    WHERE username='xxx' AND tomb=0

                如果 rows_count == 0，说明数据库中没有这个可用用户：
                - 用户名不存在
                - 或者用户已被逻辑删除(tomb != 0)

                从登录接口角度看，这就等价于“用户名或密码错误”。
                为了安全和用户体验，登录失败时通常不区分“用户名不存在”和“密码错误”，
                否则攻击者可以通过接口探测哪些用户名已经注册。

                因此这里按 PDF 要求返回 401 + “用户名或密码错误”。
            */
            if (cursor->get_rows_count() == 0) {
                response_error(resp, HttpStatusUnauthorized, "用户名或密码错误");
                return;
            }

            vector<MySQLCell> row;
            /*
                fetch_row(row) 的作用是：把当前结果集中的“下一行”真正取出来，
                并放入 row 这个 vector<MySQLCell> 中。

                前面 get_rows_count() == 0 是在判断“结果集中理论上有没有行”；
                这里 fetch_row(row) 是在判断“能不能实际取出第一行数据”。

                为什么两个判断都要写？
                1. get_rows_count() 让业务含义更清楚：0 行就是用户名不存在。
                2. fetch_row(row) 是真正读取数据前的最后一道保护。
                   如果结果集状态异常、游标位置异常、或者后续代码维护时改了查询逻辑，
                   fetch_row 失败时就不能继续访问 row[0]、row[1]。
                   否则 row 为空，继续 row[0] 会造成越界访问，程序可能崩溃。

                在当前登录场景里，fetch_row 失败同样意味着没有拿到可用于登录的用户记录。
                所以为了不泄露更多细节，也按 401 + “用户名或密码错误” 返回。
            */
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
            /*
                数据库里存的不是明文密码，而是 hash_password(原始密码, salt) 的结果。

                登录时不能直接比较用户输入的 password。
                正确流程是：
                1. 取出数据库中的 salt
                2. 用同一个 salt 对用户输入的 password 再算一次哈希
                3. 比较两次哈希是否一致

                不一致说明密码错误，按 PDF 返回 401 + “用户名或密码错误”。
            */
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

        第一阶段登录成功时，CryptoUtil::generate_token(user) 会把用户的
        id、username、createdAt 写入 JWT。

        这里虽然代码表面上是从 user.id、user.username、user.createdAt 取值，
        但这个 user 不是凭空有数据的：
        1. 先定义一个空的局部变量 User user;
        2. check_login(req, user) 读取请求头中的 Authorization: Bearer ...
        3. CryptoUtil::verify_token(token, user) 校验 token，并把 token 中的用户信息写入 user

        因此本接口不需要再查 tbl_user。
        它的含义是：从“本次请求携带的 token”中还原当前登录用户信息。
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

        /*
            这条 SQL 的含义：
            1. SELECT id, filename, size, created_at, last_update
               查询文件列表需要展示的字段：
               - id：文件编号，返回给前端时命名为 fileId，用于下载接口 /api/v1/file/{id}
               - filename：用户上传时的原始文件名
               - size：文件大小，前端用于显示 KB/MB
               - created_at：上传时间
               - last_update：最后更新时间

            2. FROM tbl_file
               从文件表查询。

            3. WHERE uid = 当前登录用户 id
               user.id 来自 token，表示当前登录用户。
               加这个条件后，用户只能看到自己的文件，不能看到其它用户的文件。

            4. ORDER BY last_update DESC, id DESC
               按最后更新时间倒序排列；如果更新时间一样，再按 id 倒序。
               这样最新上传或更新的文件会排在前面。
        */

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

        /*
            上传文件必须使用 multipart/form-data。

            前端 api.js 里 uploadRequest() 使用的是 FormData：
                const formData = new FormData();
                formData.append('file', file);

            浏览器发送 FormData 时，请求头会类似：
                Content-Type: multipart/form-data; boundary=----WebKitFormBoundary...

            如果 content_type 不是 MULTIPART_FORM_DATA，说明客户端没有按“文件上传表单”的格式发请求，
            wfrest 也无法按文件表单解析 body，所以按照 PDF 要求返回 400 + “请求格式有误”。
        */
        if (req->content_type() != MULTIPART_FORM_DATA) {
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
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        string filename = form["file"].first;
        string content = form["file"].second;
        /*
            form["file"].first 是 multipart 中 Content-Disposition 里的 filename。

            正常文件上传会带文件名，例如：
                Content-Disposition: form-data; name="file"; filename="a.txt"

            如果 filename 为空，说明这个 part 不像一个正常文件上传字段，
            可能是前端传错字段、手写 curl 时格式不对，或者上传控件没有选中文件。
            这种情况下后端无法给文件生成用户可见的名称，所以返回 400 + “请求格式有误”。
        */
        if (filename.empty()) {
            response_error(resp, HttpStatusBadRequest, "请求格式有误");
            return;
        }

        string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());
        /*
            dir 是当前用户的文件存储目录。
            例如 user.id == 3 时：
                dir = "./storage/3"

            这样不同用户的文件会放在不同目录下，目录结构更清楚。
        */
        string dir = user_storage_dir(user.id);
        /*
            real_path 是这次上传文件真正写入磁盘的位置。

            我们没有直接用原始文件名保存文件，而是用文件内容的 hashcode 保存。
            例如：
                user.id   = 3
                hashcode  = "abc123..."
                real_path = "./storage/3/abc123..."

            数据库中的 filename 仍然保存原始文件名，用于列表展示和下载时的文件名。
            数据库中的 hashcode 用来找到磁盘上的真实文件。
        */
        string real_path = storage_file_path(user.id, hashcode);

        /*
            std::error_code ec 用来接收 filesystem 操作的错误信息。

            filesystem::create_directories(dir, ec) 的作用是递归创建目录：
                dir = "./storage/3"

            如果 "./storage" 不存在，它会先创建 "./storage"；
            再创建 "./storage/3"。

            为什么不用 create_directory？
            - create_directory 只能创建一层目录
            - create_directories 可以一次创建多层目录

            为什么传 ec？
            - 不传 ec 时，创建失败可能抛异常
            - 传 ec 后，失败信息会写入 ec，代码可以用 if (ec) 做普通错误判断

            如果目录已经存在，create_directories 不会把它当成错误。
            这很适合上传接口：用户第二次上传文件时，用户目录大概率已经存在。
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

        /*
            路由注册时写的是：
                server_.GET("/api/v1/file/{id}", ...)

            这里的 {id} 是 wfrest 的“路径参数”。
            当前端请求：
                GET /api/v1/file/22

            wfrest 会把路径中的 22 提取出来，并命名为 id。
            req->param<int>("id") 的含义就是：
                取出名为 "id" 的路径参数，并转换成 int 类型。

            所以 file_id 最终等于 22。

            这个 file_id 不是用户 id，而是 tbl_file 表中的文件 id。
            后面的 SQL 还会加上 uid = 当前登录用户 id，
            防止用户通过猜测 file_id 下载其它用户的文件。
        */
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
                这属于服务器端或数据库端问题，按 PDF 返回 500。
            */
            if (cursor->get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
                return;
            }

            /*
                查询条件是：
                    WHERE id = file_id AND uid = 当前登录用户id

                rows_count == 0 可能有两种情况：
                1. 这个 file_id 根本不存在
                2. 文件存在，但不属于当前登录用户

                对下载接口来说，这两种情况都不能返回文件内容。
                PDF 要求“请求的文件不存在”返回 404 + “文件不存在”，
                所以这里统一返回 404。
            */
            if (cursor->get_rows_count() == 0) {
                response_error(resp, HttpStatusNotFound, "文件不存在");
                return;
            }

            vector<MySQLCell> row;
            /*
                和登录接口一样，get_rows_count() 判断“理论上是否有记录”，
                fetch_row(row) 是真正把第一条记录读取出来。

                如果 fetch_row 失败，后面就不能安全访问 row[0]、row[1]。
                当前接口拿不到文件记录，就等价于无法下载该文件，
                因此按 404 + “文件不存在” 返回。
            */
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
