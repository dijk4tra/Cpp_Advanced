#pragma once

#include "../../rpc_gen/cloud_disk.pb.h"

#include <functional>
#include <string>
#include <workflow/MySQLResult.h>

// 微服务端共用配置和工具。
extern const std::string DatabaseURL;
extern const int RetryMax;

// 从环境变量读取端口；缺失或非法时返回默认端口。
unsigned short get_env_port(const char* name, unsigned short default_port);

// 简单转义拼接 SQL 时会破坏字符串字面量的字符；正式项目应改用参数化 SQL。
std::string escape_sql(const std::string& s);

void set_result(cloud::disk::CommonResult* result, int code, const std::string& message);

// 同步等待单条 Workflow MySQL 任务；handler 内不得保存 cursor 引用。
bool run_mysql_query(const std::string& sql,
                     const std::function<void(protocol::MySQLResultCursor&)>& handler);
