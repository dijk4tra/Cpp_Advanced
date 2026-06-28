#include "../common/ServiceCommon.h"
#include "../../rpc_gen/cloud_disk.srpc.h"

#include <csignal>
#include <iostream>
#include <vector>
#include <workflow/WFFacilities.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

// 兼容 tbl_file.size 使用 INT 或 BIGINT 两种字段类型
static long long mysql_cell_to_file_size(const MySQLCell& cell)
{
    if (cell.is_int()) {
        return cell.as_int();
    }

    if (cell.is_ulonglong()) {
        return static_cast<long long>(cell.as_ulonglong());
    }

    return 0;
}

/*
    FileMetaService 文件元数据微服务

    它只负责 tbl_file：
    - 查询文件列表
    - 创建文件元数据
    - 查询下载前所需的 filename/hashcode

    它不负责：
    - HTTP
    - multipart/form-data
    - 临时文件
    - RabbitMQ
    - OSS
*/
class FileMetaServiceImpl : public cloud::disk::FileMetaService::Service {
public:
    /*
        ListFiles 对应 HTTP 网关中的 GET /api/v1/files。
    */
    void ListFiles(cloud::disk::ListFilesRequest* request,
                   cloud::disk::ListFilesResponse* response,
                   srpc::RPCContext*) override
    {
        /*
            取出当前登录用户 id。
            这个 user_id 来自网关的 AuthService.VerifyToken 结果。
        */
        int user_id = request->user_id();

        if (user_id <= 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        string sql =
            "SELECT id, filename, size, created_at, last_update "
            "FROM tbl_file "
            "WHERE uid=" + to_string(user_id) + " "
            "ORDER BY last_update DESC, id DESC;"; // 最近更新的文件排在前面

        bool db_error = false; // db_error 表示 SELECT 是否异常

        // 执行查询
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            // SELECT 成功时状态应该是 MYSQL_STATUS_GET_RESULT
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            vector<MySQLCell> row; // row 保存每次 fetch_row() 读出的一行文件记录

            /*
                文件列表为空不是错误。
                如果没有文件，while 一次也不会进入，response->files 仍然是空 repeated 字段
            */
            while (cursor.fetch_row(row)) {
                /*
                    add_files() 会在 repeated files 后面追加一个 FileInfo，
                    并返回这个新元素的可写指针。
                */
                cloud::disk::FileInfo* file = response->add_files();

                // 按 SELECT 字段顺序写入 FileInfo
                file->set_file_id(row[0].as_int());
                file->set_filename(row[1].as_string());
                file->set_size(mysql_cell_to_file_size(row[2]));
                file->set_created_at(row[3].as_string());
                file->set_updated_at(row[4].as_string());
            }
        });

        if (!query_ok || db_error) { // 查询失败时返回内部错误
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        // 设置业务成功
        set_result(response->mutable_result(), 0, "获取文件列表成功");
    }

    /*
        CreateFile 对应上传流程中的“创建文件元数据”步骤。

        API Gateway 负责接收文件内容和保存临时文件；
        FileMetaService 只把 uid/filename/hashcode/size 写入 tbl_file。
    */
    void CreateFile(cloud::disk::CreateFileRequest* request,
                    cloud::disk::CreateFileResponse* response,
                    srpc::RPCContext*) override
    {
        // 从请求中取出字段
        int user_id = request->user_id();
        string filename = request->filename();
        string hashcode = request->hashcode();
        long long size = request->size();

        // 做基础参数校验
        if (user_id <= 0 || filename.empty() || hashcode.empty() || size < 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        string sql =
            "INSERT INTO tbl_file (uid, filename, hashcode, size) VALUES (" +
            to_string(user_id) + ", '" +
            escape_sql(filename) + "', '" +
            escape_sql(hashcode) + "', " +
            to_string(size) + ");";

        int file_id = 0; // file_id 保存新插入文件记录的 id
        int cursor_status = MYSQL_STATUS_ERROR; // cursor_status 保存 INSERT 结果状态

        // 执行 INSERT
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            // INSERT 成功时状态应该是 MYSQL_STATUS_OK
            cursor_status = cursor.get_cursor_status();
            // 获取自增 id
            file_id = static_cast<int>(cursor.get_insert_id());
        });

        // 文件元数据写入失败时，网关会删除临时文件并返回 500
        if (!query_ok || cursor_status != MYSQL_STATUS_OK) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        // 设置业务成功
        set_result(response->mutable_result(), 0, "上传成功");
        response->set_file_id(file_id);
        response->set_filename(filename);
    }

    /*
        GetFileForDownload 对应下载前的元数据查询。

        网关用返回的 filename 设置下载文件名，
        用 hashcode 从 OSS 下载对象内容。
    */
    void GetFileForDownload(cloud::disk::GetFileForDownloadRequest* request,
                            cloud::disk::GetFileForDownloadResponse* response,
                            srpc::RPCContext*) override
    {
        // 取出用户 id 和文件 id
        int user_id = request->user_id();
        int file_id = request->file_id();

        if (user_id <= 0 || file_id <= 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        /*
            查询 filename/hashcode。
            同时带上 uid 条件，防止用户通过猜 file_id 下载别人的文件。
        */
        string sql =
            "SELECT filename, hashcode "
            "FROM tbl_file "
            "WHERE id=" + to_string(file_id) + " AND uid=" + to_string(user_id) + " "
            "LIMIT 1;";

        bool found = false;    // found 表示是否找到文件记录
        bool db_error = false; // db_error 表示 SELECT 是否异常

        // 保存查询出的文件名和 hashcode
        string filename;
        string hashcode;

        // 执行查询
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            // SELECT 成功时状态应该是 MYSQL_STATUS_GET_RESULT
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            vector<MySQLCell> row;

            // 没有行表示文件不存在或不属于当前用户
            if (!cursor.fetch_row(row)) {
                found = false;
                return;
            }

            // 找到了文件记录
            found = true;

            // 拷贝 filename/hashcode
            filename = row[0].as_string();
            hashcode = row[1].as_string();
        });

        if (!query_ok || db_error) { // 查询失败返回 500
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        if (!found) { // 文件不存在返回 404
            set_result(response->mutable_result(), 404, "文件不存在");
            return;
        }

        // 设置业务成功
        set_result(response->mutable_result(), 0, "获取下载信息成功");

        // 返回下载所需元数据
        response->set_filename(filename);
        response->set_hashcode(hashcode);
    }
};

static WFFacilities::WaitGroup wait_group(1);

static void sig_handler(int)
{
    wait_group.done();
}

int main()
{
    // 初始化 protobuf
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // 注册 Ctrl+C 信号
    signal(SIGINT, sig_handler);

    unsigned short port = get_env_port("FILEMETA_SERVICE_PORT", 9003);

    // 创建 srpc server
    srpc::SRPCServer server;
    // 创建文件元数据服务实现对象
    FileMetaServiceImpl service;
    // 注册 FileMetaService
    server.add_service(&service);

    // 启动服务
    if (server.start(port) == 0) {
        cout << "[FileMetaService] listening on " << port << endl;

        wait_group.wait(); // 阻塞等待退出信号
        server.stop();     // 停止 srpc server
    } else {
        cerr << "[FileMetaService] start FAILED on port " << port << endl;
    }

    // 释放 protobuf 资源
    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}
