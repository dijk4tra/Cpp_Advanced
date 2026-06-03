/**
 * [[回声服务器]]
 * 我们将使用Workflow创建一个HTTP服务器，它会将接收到的HTTP请求，以html格式返回给客户端。
 * 此外，它还需要完成下面这些功能：
 *   - 程序会打印出客户端的IP地址，请求序号（当前TCP连接上的第几次请求）。
 *   - 当一个TCP连接完成了10次请求，服务器主动关闭连接。
 *   - 按下Ctrl + C程序能够正常结束，资源都会被回收。
 */
#include "common.h"
#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFServer.h>

using namespace std;
using namespace protocol;
