#include "../common/ServiceCommon.h"
#include "../../rpc_gen/cloud_disk.srpc.h"

#include <csignal>
#include <iostream>
#include <vector>
#include <workflow/WFFacilities.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

// 兼容 tbl_file.size 使用 INT 或 BIGINT 两种字段类型。
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

// FileMetaService 只负责 tbl_file 元数据，不处理 HTTP、临时文件、RabbitMQ 或 OSS。
class FileMetaServiceImpl : public cloud::disk::FileMetaService::Service {
public:
    void ListFiles(cloud::disk::ListFilesRequest* request,
                   cloud::disk::ListFilesResponse* response,
                   srpc::RPCContext*) override
    {
        int user_id = request->user_id();

        if (user_id <= 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        string sql =
            "SELECT id, filename, size, created_at, last_update "
            "FROM tbl_file "
            "WHERE uid=" + to_string(user_id) + " "
            "ORDER BY last_update DESC, id DESC;";

        bool db_error = false;

        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            vector<MySQLCell> row;

            while (cursor.fetch_row(row)) {
                cloud::disk::FileInfo* file = response->add_files();

                file->set_file_id(row[0].as_int());
                file->set_filename(row[1].as_string());
                file->set_size(mysql_cell_to_file_size(row[2]));
                file->set_created_at(row[3].as_string());
                file->set_updated_at(row[4].as_string());
            }
        });

        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        set_result(response->mutable_result(), 0, "获取文件列表成功");
    }

    void CreateFile(cloud::disk::CreateFileRequest* request,
                    cloud::disk::CreateFileResponse* response,
                    srpc::RPCContext*) override
    {
        int user_id = request->user_id();
        string filename = request->filename();
        string hashcode = request->hashcode();
        long long size = request->size();

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

        int file_id = 0;
        int cursor_status = MYSQL_STATUS_ERROR;

        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            cursor_status = cursor.get_cursor_status();
            file_id = static_cast<int>(cursor.get_insert_id());
        });

        if (!query_ok || cursor_status != MYSQL_STATUS_OK) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        set_result(response->mutable_result(), 0, "上传成功");
        response->set_file_id(file_id);
        response->set_filename(filename);
    }

    void GetFileForDownload(cloud::disk::GetFileForDownloadRequest* request,
                            cloud::disk::GetFileForDownloadResponse* response,
                            srpc::RPCContext*) override
    {
        int user_id = request->user_id();
        int file_id = request->file_id();

        if (user_id <= 0 || file_id <= 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        // uid 条件防止用户通过猜 file_id 下载其他用户文件。
        string sql =
            "SELECT filename, hashcode "
            "FROM tbl_file "
            "WHERE id=" + to_string(file_id) + " AND uid=" + to_string(user_id) + " "
            "LIMIT 1;";

        bool found = false;
        bool db_error = false;

        string filename;
        string hashcode;

        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            vector<MySQLCell> row;

            if (!cursor.fetch_row(row)) {
                found = false;
                return;
            }

            found = true;

            filename = row[0].as_string();
            hashcode = row[1].as_string();
        });

        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        if (!found) {
            set_result(response->mutable_result(), 404, "文件不存在");
            return;
        }

        set_result(response->mutable_result(), 0, "获取下载信息成功");
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
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    signal(SIGINT, sig_handler);

    unsigned short port = get_env_port("FILEMETA_SERVICE_PORT", 9003);

    srpc::SRPCServer server;
    FileMetaServiceImpl service;

    server.add_service(&service);

    if (server.start(port) == 0) {
        cout << "[FileMetaService] listening on " << port << endl;

        wait_group.wait();
        server.stop();
    } else {
        cerr << "[FileMetaService] start FAILED on port " << port << endl;
    }

    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}
