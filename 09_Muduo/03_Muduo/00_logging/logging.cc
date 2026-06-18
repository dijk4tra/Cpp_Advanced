#include "common.h"
#include <muduo/base/Logging.h>
#include <muduo/base/LogFile.h>

int main()
{
    int error_code = 2;
    // errno = 2;

    muduo::Logger::setLogLevel(muduo::Logger::WARN);


    LOG_TRACE << "这是追踪信息，只有开启 TRACE 级别才会输出";
    LOG_DEBUG << "这是调试信息，id=" << 9527;
    LOG_INFO << "服务器已启动，监听端口：8080";
    LOG_WARN << "警告：磁盘使用率超过 80%";
    LOG_ERROR << "写入数据库失败，错误码：" << error_code;
    LOG_SYSERR << "日志级别为 ERROR，会输出 strerror(errno)，系统调用出错时使用";
    // LOG_FATAL << "出现致命错误时使用，程序会调用 abort() 异常退出";
    // LOG_SYSFATAL << "出现致命错误时使用，日志级别为 FATAL，会输出 strerror(errno)";
}
