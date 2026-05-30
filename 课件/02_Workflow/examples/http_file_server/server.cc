#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <string>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

#include "CryptoUtil.h"

using namespace std;
using namespace protocol;

struct Context {
    HttpResponse* httpResp;
    string username;
    string password;
};

void register_callback(WFMySQLTask* task)
{
    Context* ctx = static_cast<Context*>(series_of(task)->get_context());

    /* MySQL 任务失败: 比如MySQL服务宕机了 */
    if (task->get_state() != WFT_STATE_SUCCESS) {
        ctx->httpResp->set_status_code("500");
        ctx->httpResp->append_output_body_nocopy("<html>Internal Server Error</html>");
        return;
    }

    if (task->get_resp()->get_packet_type() == MYSQL_PACKET_ERROR) {
        /* SQL语句执行失败 */
        ctx->httpResp->set_status_code("400");
        ctx->httpResp->append_output_body_nocopy("<html>用户名已存在</html>");
        return;
    }

    // SQL语句执行成功 (如果是Insert语句，则表明添加用户成功)
    ctx->httpResp->append_output_body_nocopy("恭喜您，注册成功！");
}

void do_register(WFHttpTask* httpTask)
{
    // 1. 获取用户名和密码
    HttpRequest* req = httpTask->get_req();

    const void* body;
    size_t size;
    req->get_parsed_body(&body, &size);

    /* name=<name>&password=<password> */
    string content = static_cast<const char*>(body);
    auto eq1 = content.find_first_of('=');
    auto eq2 = content.find_last_of('=');
    auto amp = content.find('&');
    string username = content.substr(eq1 + 1, amp - eq1 - 1);
    string password = content.substr(eq2 + 1);
    cout << "username: " << username << ", password: " << password << endl;

    // 2. 添加用户 (MySQL任务)
    string salt = CryptoUtil::generate_salt();
    string hashcode = CryptoUtil::hash_password(password, salt);
    // 拼接SQL语句
    string sql = "INSERT INTO tbl_user (username, password, salt) VALUES ('" + username + "', '" + hashcode + "', '" + salt + "');";
    cout << "[SQL] " << sql << endl; /* 日志 */

    string url = "mysql://root:1234@localhost/demo";
    WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(url, 3, register_callback);
    mysqlTask->get_req()->set_query(sql);

    Context* ctx = new Context { httpTask->get_resp(), username, password };
    SeriesWork* series = series_of(httpTask);
    series->set_context(ctx);

    series->set_callback([ctx](const SeriesWork*) {
        delete ctx;
    });

    series->push_back(mysqlTask);
}

// 名字最好和业务相关，不要和技术相关
void login_callback(WFMySQLTask* task)
{
    MySQLResponse* resp = task->get_resp();
    Context* ctx = static_cast<Context*>(series_of(task)->get_context());
    // 如果MySQL任务失败，返回500
    if (task->get_state() != WFT_STATE_SUCCESS || resp->get_packet_type() == MYSQL_PACKET_ERROR) {
        ctx->httpResp->set_status_code("500");
        ctx->httpResp->append_output_body_nocopy("<html>Internal Server Error</html>");
        return;
    }

    // 可用MySQLResultCursor遍历结果集
    MySQLResultCursor cursor(resp);

    vector<MySQLCell> record; /* 数据库中的一行又称为一条记录(record) */
    if (cursor.fetch_row(record)) {
        User user;
        user.id = record[0].as_int();
        user.username = record[1].as_string();
        user.password = record[2].as_string();
        user.salt = record[3].as_string();
        user.createdAt = record[4].as_datetime();
        // 根据用户输入的密码和查询到的盐值，生成哈希值
        string hashcode = CryptoUtil::hash_password(ctx->password, user.salt);
        // 如果生成的哈希值和数据库中的哈希值相等
        if (hashcode == user.password) {
            string body = user.username + "，欢迎您\n";
            ctx->httpResp->append_output_body(body);
            ctx->httpResp->append_output_body(CryptoUtil::generate_token(user));
            return;
        }
    }
    /* 用户不存在: 空结果集或输入的密码错误 */
    ctx->httpResp->set_status_code("400");
    ctx->httpResp->append_output_body_nocopy("用户名或密码错误");
}

void do_login(WFHttpTask* httpTask)
{
    // 1. 解析请求: 获取用户名和密码
    HttpRequest* req = httpTask->get_req();
    const void* body;
    size_t size;
    req->get_parsed_body(&body, &size);

    // name=<name>&password=<password>
    string content = static_cast<const char*>(body);
    auto eq1 = content.find_first_of('=');
    auto eq2 = content.find_last_of('=');
    auto amp = content.find('&');
    string username = content.substr(eq1 + 1, amp - eq1 - 1);
    string password = content.substr(eq2 + 1);
    cout << "username: " << username << ", password: " << password << endl;

    // 2. 校验 username 和 password
    string url = "mysql://root:1234@localhost/demo";
    string sql = "SELECT * from tbl_user WHERE username='" + username + "'";
    cout << "[SQL] " << sql << endl;

    WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(url, 3, login_callback);
    mysqlTask->get_req()->set_query(sql);

    SeriesWork* series = series_of(httpTask);
    // 设置序列的上下文
    Context* ctx = new Context { httpTask->get_resp(), username, password };
    series->set_context(ctx);

    series->set_callback([ctx](const SeriesWork*) {
        delete ctx;
    });
    // 将mysqlTask添加到httpTask所在的序列中
    series->push_back(mysqlTask);
}

void pread_callback(WFFileIOTask* preadTask, HttpResponse* resp, string filename)
{
    /**
     * struct FileIOArgs
     * {
     *     int fd;
     *     void* buf;
     *     size_t count;
     *     off_t offset;
     * };
     */
    FileIOArgs* args = preadTask->get_args();
    close(args->fd); // 关闭文件描述符

    long bytes = preadTask->get_retval();

    if (preadTask->get_state() != WFT_STATE_SUCCESS) {
        resp->set_status_code("500");
        resp->append_output_body_nocopy("<html>Internal Server Error.</html>");
    } else {
        // 设置 Content-Disposition 标头
        resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);
        resp->append_output_body_nocopy(args->buf, bytes);
    }
}

void do_send(WFHttpTask* httpTask)
{
    HttpRequest* req = httpTask->get_req();
    HttpResponse* resp = httpTask->get_resp();

    HttpHeaderCursor cursor(req);
    string value;
    cursor.find("Authorization", value);

    /* Authorization: Bearer <token> 其中 Bearer 是Http中的一种认证方案 */
    string token = value.substr(value.find(' ') + 1);
    User user;
    if (!CryptoUtil::verify_token(token, user)) {
        resp->set_status_code("401");
        resp->append_output_body_nocopy("<html>401 Unauthorized</html>");
        return;
    }
    cout << "认证通过: " << user.username << endl;

    // 解析uri，获取文件路径
    string uri = req->get_request_uri(); // <path>?<query>
    string path = uri.substr(0, uri.find('?'));
    if (path == "/") {
        path += "index.html";
    }
    path = "resources" + path;

    // 获取文件名
    string filename = path.substr(path.find_last_of('/') + 1);

    // 判断文件是否可访问(R_OK)
    if (access(path.c_str(), R_OK) == -1) { // F_OK, R_OK, W_OK, X_OK
        resp->set_status_code("404");
        resp->append_output_body_nocopy("<html>404 Not Found!</html>");
        return;
    }
    // 文件一定存在，并且有读权限
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) { // 打开失败
        resp->set_status_code("500");
        resp->append_output_body_nocopy("<html>Internal Server Error!</html>");
        return;
    }

    // 打开成功，获取文件的大小
    size_t size = lseek(fd, 0, SEEK_END);
    // 在堆上申请 buf
    void* buf = malloc(size);
    assert(buf != nullptr && "malloc failed!");

    // 这里只能使用值捕获，不能使用引用捕获！因为引用捕获，引用的是栈上的指针变量buf
    httpTask->set_callback([buf](WFHttpTask* task) {
        // 在httpTask的回调函数里释放buf
        free(buf);
    });

    WFFileIOTask* preadTask = WFTaskFactory::create_pread_task(
        fd,
        buf,
        size,
        0,
        bind(pread_callback, placeholders::_1, resp, filename));

    // 将preadTask添加到httpTask所在的序列中
    series_of(httpTask)->push_back(preadTask);
}

void process(WFHttpTask* httpTask)
{
    // 1. 解析请求
    HttpRequest* req = httpTask->get_req();
    HttpResponse* resp = httpTask->get_resp();

    // 获取路径 path
    string method = req->get_method();
    string uri = req->get_request_uri();
    string path;
    auto pos = uri.find('?');
    if (pos == string::npos) {
        path = uri;
    } else {
        path = uri.substr(0, pos);
    }

    // 2. 分发请求
    if (method == "POST") {
        if (path == "/login") {
            do_login(httpTask);
        } else if (path == "/register") {
            do_register(httpTask);
        } else {
            resp->set_status_code("404");
            resp->append_output_body_nocopy("<html>404 Not Found</html>");
        }
    } else if (method == "GET") {
        do_send(httpTask);
    } else {
        // 不允许使用其它方法
        resp->set_status_code("405"); // Method Not Allowed
    }
}

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

int main()
{
    // 注册信号处理函数
    // 用户按下Ctrl-C终止程序
    signal(SIGINT, sig_handler);

    WFHttpServer server(process);

    if (server.start(8888) == 0) {
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}
