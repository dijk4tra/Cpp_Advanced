/*
 * 静态资源服务器
 *
 * 功能：
 * 1. POST /register 注册用户，把“加盐后的密码哈希值”存入 MySQL。
 * 2. POST /login 登录用户，登录成功后把 Token 放在响应体中返回。
 * 3. GET /xxx 下载 resources 目录中的静态资源，下载前必须验证 Token。
 *
 */
#include "common.h"
#include "CryptoUtil.h"

#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/MySQLMessage.h>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>
#include <workflow/http_parser.h>

using namespace std;
using namespace std::placeholders;
using namespace protocol;

// MySQL 连接地址
static const string MYSQL_URL = "mysql://root:123456@127.0.0.1:3306/demo?character_set=utf8mb4";

// HTTP 服务监听端口
static const int SERVER_PORT = 8888;

// waitGroup 用来让 main 函数一直阻塞，直到按 Ctrl+C 退出
WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

void set_text_response(HttpResponse* resp, const string& code, const string& body)
{
    // 设置状态码，例如 "200"、"401"、"404"
    resp->set_status_code(code);

    // 统一返回普通文本，方便 curl 直接查看
    resp->set_header_pair("Content-Type", "text/plain; charset=utf-8");

    // 把字符串追加到响应体
    resp->append_output_body(body);
}

void set_unauthorized(HttpResponse* resp)
{
    // 未登录，或者 Token 超时，返回 401 unauthorized
    resp->set_status_code("401");
    resp->set_reason_phrase("Unauthorized");
    resp->set_header_pair("Content-Type", "text/plain; charset=utf-8");
    resp->append_output_body("401 unauthorized\n");
}

string url_decode(const string& text)
{
    string result;

    // application/x-www-form-urlencoded 中，+ 表示空格
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            result += ' ';
        } else if (text[i] == '%' && i + 2 < text.size()
                   && isxdigit(text[i + 1]) && isxdigit(text[i + 2])) {
            // %E4%B8%AD 这种格式表示一个字节的十六进制值
            string hex = text.substr(i + 1, 2);
            char ch = (char)strtol(hex.c_str(), NULL, 16);
            result += ch;
            i += 2;
        } else {
            result += text[i];
        }
    }

    return result;
}

map<string, string> parse_form(const string& body)
{
    map<string, string> form;
    size_t begin = 0;

    // 表单格式示例: username=alice&password=123456
    while (begin < body.size()) {
        // 找到当前字段的结尾
        size_t end = body.find('&', begin);
        if (end == string::npos) {
            end = body.size();
        }

        // 取出一个 key=value 片段
        string item = body.substr(begin, end - begin);

        // 找到 key 和 value 中间的等号
        size_t equalPos = item.find('=');
        if (equalPos != string::npos) {
            string key = url_decode(item.substr(0, equalPos));
            string value = url_decode(item.substr(equalPos + 1));
            form[key] = value;
        }

        // 继续处理下一个字段
        begin = end + 1;
    }

    return form;
}

string get_request_body(HttpRequest* req)
{
    const void* body = NULL;
    size_t size = 0;

    // workflow 已经把 HTTP 请求体解析好了，这里只需要取出来
    if (req->get_parsed_body(&body, &size) && body != NULL) {
        return string((const char*)body, size);
    }

    return "";
}

string sql_escape(const string& text)
{
    string result;

    // 这个函数只做最基本 SQL 字符串转义
    // 例如 O'Reilly 会变成 O''Reilly，避免单引号提前结束 SQL 字符串
    for (char ch : text) {
        if (ch == '\'') {
            result += "''";
        } else if (ch == '\\') {
            result += "\\\\";
        } else {
            result += ch;
        }
    }

    return result;
}

string get_header_value(HttpRequest* req, const string& name)
{
    string value;
    HttpHeaderCursor cursor(req);

    // 在请求头里查找指定名字的头部字段
    if (cursor.find(name, value)) {
        return value;
    }

    return "";
}

string get_token_from_request(HttpRequest* req, const string& uri)
{
    // 常见写法：Authorization: Bearer xxxxx
    string authorization = get_header_value(req, "Authorization");
    string bearer = "Bearer ";
    if (authorization.compare(0, bearer.size(), bearer) == 0) {
        return authorization.substr(bearer.size());
    }

    // 为了方便测试, 也支持 Authorization: xxxxx
    if (!authorization.empty()) {
        return authorization;
    }

    // 再支持 Token: xxxxx
    string tokenHeader = get_header_value(req, "Token");
    if (!tokenHeader.empty()) {
        return tokenHeader;
    }

    // 最后支持 /index.html?token=xxxxx，方便直接在浏览器地址栏测试
    size_t queryPos = uri.find('?');
    if (queryPos != string::npos) { // string::npos为特殊常量, 含义为"没有找到"
        string query = uri.substr(queryPos + 1);
        map<string, string> queryMap = parse_form(query);
        if (queryMap.count("token") != 0) {
            return queryMap["token"];
        }
    }

    return "";
}

string get_resource_root()
{
    static string root;
    // 第一次调用时判断 resources 目录在哪里
    if (root.empty()) {
        struct stat statbuf;

        // 如果从项目根目录启动，resources 就在当前目录下
        if (stat("resources", &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
            root = "resources";
        } else {
            // 如果进入 Static_Resource_Server 目录启动，则 resources 在上一层
            root = "../resources";
        }
    }

    return root;
}

void register_callback(WFMySQLTask* task, HttpResponse* resp, string username)
{
    // 1. 先看 MySQL 任务本身是否执行成功
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        set_text_response(resp, "500", "register failed: mysql task error\n");
        return;
    }

    // 2. 再看 MySQL 返回的是 OK 包还是错误包
    MySQLResponse* mysqlResp = task->get_resp();
    if (mysqlResp->is_error_packet()) {
        cerr << "mysql error: " << mysqlResp->get_error_msg() << endl;

        // 用户名有 UNIQUE 约束，重复注册会报 1062
        if (mysqlResp->get_error_code() == 1062) {
            set_text_response(resp, "409", "register failed: username already exists\n");
        } else {
            set_text_response(resp, "500", "register failed: mysql error\n");
        }
        return;
    }

    // 3. 注册成功
    cout << "[register] " << username << endl;
    set_text_response(resp, "200", "register success\n");

}

void login_callback(WFMySQLTask* task, HttpResponse* resp, string password)
{
    // 1. 检查 MySQL 任务状态
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        set_text_response(resp, "500", "login failed: mysql task error\n");
        return;
    }

    // 2. 检查 SQL 是否报错
    MySQLResponse* mysqlResp = task->get_resp();
    if (mysqlResp->is_error_packet()) {
        cerr << "mysql error: " << mysqlResp->get_error_msg() << endl;
        set_text_response(resp, "500", "login failed: mysql error\n");
        return;
    }

    // 3. 读取查询结果
    MySQLResultCursor cursor(mysqlResp);
    vector<MySQLCell> row;

    // 没有查到用户，说明用户名不存在或者 tomb=1
    if (!cursor.fetch_row(row)) {
        set_unauthorized(resp);
        return;
    }

    // SELECT id, username, password, salt, created_at ...
    User user;
    user.id = row[0].as_int();
    user.username = row[1].as_string();
    user.hashcode = row[2].as_string();
    user.salt = row[3].as_string();
    if (!row[4].is_null()) {
        user.createdAt = row[4].as_string();
    }

    // 4. 用“客户端传来的明文密码 + 数据库里的盐值”重新计算哈希
    string hashcode = CryptoUtil::hash_password(password, user.salt);

    // 5. 如果两次哈希不一样，说明密码错误
    if (hashcode != user.hashcode) {
        set_unauthorized(resp);
        return;
    }

    // 6. 密码正确，生成 30 分钟有效的 Token
    string token = CryptoUtil::generate_token(user);

    cout << "[login] " << user.username << endl;

    // 响应体只返回 Token，客户端后续请求带上它即可
    set_text_response(resp, "200", token + "\n");
}

void pread_callback(WFFileIOTask* task, HttpResponse* resp, string filename)
{
    // 1. 获取文件 IO 任务的参数
    FileIOArgs* args = task->get_args();

    // 文件描述符用完后一定要关闭
    close(args->fd);

    // get_retval() 是这次 pread 实际读到的字节数
    long bytesRead = task->get_retval();

    // 2. 判断文件 IO 是否成功
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        resp->set_status_code("500");
        resp->append_output_body("<html>500 Server Internal Error.</html>");
        return;
    }

    // 3. 设置下载文件名
    resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);

    // 4. 把读到的文件内容直接放进响应体
    // append_output_body_nocopy 不复制 buf，所以 buf 要等 HTTP 响应发送结束后再释放
    resp->append_output_body_nocopy(args->buf, bytesRead);
}

void handle_register(WFHttpTask* httpTask)
{
    HttpRequest* req = httpTask->get_req();
    HttpResponse* resp = httpTask->get_resp();

    // 1. 读取表单参数
    string body = get_request_body(req);
    map<string, string> form = parse_form(body);
    string username = form["username"];
    string password = form["password"];

    // 2. 简单校验，用户名和密码不能为空
    if (username.empty() || password.empty()) {
        set_text_response(resp, "400", "username and password are required\n");
        return;
    }

    // 3. 为每个用户生成自己的随机盐值
    string salt = CryptoUtil::generate_salt(16);

    // 4. 存入数据库的不是明文密码，而是 password + salt 计算后的哈希
    string hashcode = CryptoUtil::hash_password(password, salt);

    // 5. 拼接 INSERT SQL
    string query = "INSERT INTO tbl_user (username, password, salt) VALUES ('"
                   + sql_escape(username) + "', '"
                   + sql_escape(hashcode) + "', '"
                   + sql_escape(salt) + "')";

    cout << "[sql] " << query << endl;

    // 6. 创建 MySQL 任务
    WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
        MYSQL_URL,
        3,
        bind(register_callback, _1, resp, username)
    );

    // 7. 给 MySQL 任务设置 SQL
    mysqlTask->get_req()->set_query(query);

    // 8. 把 MySQL 任务加入当前 HTTP 请求所在的序列
    // workflow 会先执行 MySQL 任务，再发送 HTTP 响应
    series_of(httpTask)->push_back(mysqlTask);
}

void handle_login(WFHttpTask* httpTask)
{
    HttpRequest* req = httpTask->get_req();
    HttpResponse* resp = httpTask->get_resp();

    // 1. 读取表单数据
    string body = get_request_body(req);
    map<string, string> form = parse_form(body);
    string username = form["username"];
    string password = form["password"];

    // 2. 用户名和密码不能为空
    if (username.empty() || password.empty()) {
        set_text_response(resp, "400", "username and password are required\n");
        return;
    }

    // 3. 根据用户名查询用户
    string query = "SELECT id, username, password, salt, created_at "
                   "FROM tbl_user "
                   "WHERE username = '" + sql_escape(username) + "' AND tomb = 0 "
                   "LIMIT 1";

    cout << "[sql] " << query << endl;

    // 4. 创建 MySQL 查询任务
    WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
        MYSQL_URL,
        3,
        bind(login_callback, _1, resp, password)
    );

    // 5. 设置 SQL
    mysqlTask->get_req()->set_query(query);

    // 6. 加入当前 HTTP 请求的序列
    series_of(httpTask)->push_back(mysqlTask);
}

void handle_static_file(WFHttpTask* httpTask, string path)
{
    HttpResponse* resp = httpTask->get_resp();

    // 1. / 映射到 /index.html
    if (path == "/") {
        path += "index.html";
    }

    // 2. 只允许访问 resources 目录下的文件
    // 如果 URL 中包含 ..，就可能访问到 resources 外面的文件，这里直接拒绝
    if (path.find("..") != string::npos) {
        set_text_response(resp, "403", "403 forbidden\n");
        return;
    }

    // 3. 拼出磁盘路径
    string filePath = get_resource_root() + path;
    cout << "[file] " << filePath << endl;

    // 4. 取得文件名，用于 Content-Disposition
    size_t pos = filePath.find_last_of("/");
    string filename = filePath.substr(pos + 1);

    // 5. 打开文件
    int fd = open(filePath.c_str(), O_RDONLY);
    if (fd == -1) {
        set_text_response(resp, "404", "<html>404 Not Found.</html>");
        return;
    }

    // 6. 获取文件大小，并确认它是普通文件
    struct stat statbuf;
    fstat(fd, &statbuf);
    if (!S_ISREG(statbuf.st_mode)) {
        close(fd);
        set_text_response(resp, "404", "<html>404 Not Found.</html>");
        return;
    }

    size_t size = statbuf.st_size;
    cout << "[size] " << size << endl;

    // 7. 空文件不需要创建 pread 任务
    if (size == 0) {
        close(fd);
        resp->set_status_code("200");
        resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);
        return;
    }

    // 8. 为文件内容申请一块内存
    char* buf = (char*)malloc(size);
    assert(buf != NULL && "malloc failed");

    // 9. HTTP 响应发送结束后释放 buf
    httpTask->set_callback([buf](WFHttpTask*) {
       free(buf);
    });

    // 10. 创建异步 pread 任务
    WFFileIOTask* preadTask = WFTaskFactory::create_pread_task(
        fd,
        buf,
        size,
        0,
        bind(pread_callback, _1, resp, filename));

    // 11. 把 pread 任务加入当前 HTTP 请求的序列
    series_of(httpTask)->push_back(preadTask);
}


void process(WFHttpTask* httpTask)
{
    // 1. 解析 HTTP 请求
    HttpRequest* req = httpTask->get_req();
    HttpResponse* resp = httpTask->get_resp();
    string method = req->get_method();
    string uri = req->get_request_uri();

    // 2. 设置响应头
    resp->set_header_pair("Server", "Static Resource Server");

    // 3. 把 URL 中 ? 后面的查询字符串去掉，只留下路径
    size_t queryPos = uri.find('?');
    string path = uri.substr(0, queryPos);

    cout << "[request] " << method << " " << uri << endl;

    // 4. 注册接口：POST /register
    if (path == "/register") {
        if (method != "POST") {
            set_text_response(resp, "405", "method not allowed\n");
            return;
        }
    }

    // 5. 登录接口：POST /login
    if (path == "/login") {
        if (method != "POST") {
            set_text_response(resp, "405", "method not allowed\n");
            return;
        }
        handle_login(httpTask);
        return;
    }

    // 6. 其它路径都当作静态资源下载，只允许 GET
    if (method != "GET") {
        set_text_response(resp, "405", "method not allowed\n");
        return;
    }

    // 7. 从请求头或者查询字符串里取 Token
    string token = get_token_from_request(req, uri);

    // 8. 校验 Token，如果失败，说明未登录或者已经超时
    User user;
    if (token.empty() || !CryptoUtil::verify_token(token, user)) {
        set_unauthorized(resp);
        return;
    }

    // 9. Token 校验成功，说明用户已经登录
    cout << "[auth] user_id=" << user.id << ", username=" << user.username << endl;

    // 10. 下载静态文件
    handle_static_file(httpTask, path);
}


int main()
{
    // 让 rand() 每次运行时产生不同的随机序列, 用于生成 salt
    srand(time(NULL));

    // 注册 Ctrl+C 信号处理函数
    signal(SIGINT, sig_handler);

    // 创建 HTTP 服务器
    WFHttpServer server(process);

    // 启动服务器
    if (server.start(SERVER_PORT) == 0) {
        cout << "Static resource server is running at http://127.0.0.1:"
             << SERVER_PORT << endl;

        // 主线程阻塞在这里，直到 Ctrl+C 触发 waitGroup.done()
        waitGroup.wait();
        // 停止服务器
        server.stop();
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}
