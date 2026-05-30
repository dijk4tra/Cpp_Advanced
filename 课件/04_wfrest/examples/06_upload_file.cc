#include <iostream>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <wfrest/PathUtil.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signum)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    HttpServer server;

    // curl -v -X POST "ip:port/upload-file" -F "file=@demo.txt;
    // filename=../demo.txt" -H "Content-Type: multipart/form-data"
    // 1. 获取文件的名字
    // 2. 获取文件的内容
    // 3. 保存文件
    server.POST("/upload-file", [](const HttpReq* req, HttpResp* resp) {
        // 校验请求体的类型：multipart/form-data
        if (req->content_type() != MULTIPART_FORM_DATA) {
            resp->set_status(HttpStatusBadRequest); // 400: BadRequest
            return;
        }
        //                   key         filename, content
        // using Form = map<string, pair<string, string>>
        const Form& form = req->form();

        for (const auto& [key, file] : form) {
            const string& filename = file.first;
            const string& content = file.second;
#ifdef DEBUG
            std::cout << filename << ": " << content << endl;
#endif
            // filename 不应该被信任，不应该被应用程序盲目使用。
            // 参见 MDN 上的 Content-Disposition
            // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Disposition#directives
            // 应去除路径信息，并应用服务器文件系统规则

            // resp->Save(filename, std::move(content));  // Wrong: 有安全隐患
            string basename = PathUtil::base(filename);
            // resp->Save(basename, std::move(content));
            resp->Save(basename, std::move(content), "Save " + basename + " Success\n");
        }
    });

    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
