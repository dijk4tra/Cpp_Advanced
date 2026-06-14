#include "../common/ServiceCommon.h"
#include "../../rpc_gen/cloud_disk.srpc.h"

#include <csignal>
#include <iostream>
#include <vector>
#include <workflow/WFFacilities.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

/*
    把 MySQLCell 中的文件大小转换成 int64。

    为什么不直接用 as_ulonglong()？
    - 当前 tbl_file.size 字段在课程项目中通常是普通 INT。
    - Workflow 的 MySQLCell::as_ulonglong() 只接受 MYSQL_TYPE_LONGLONG。
    - 如果对普通 INT 调 as_ulonglong()，它会返回 (unsigned long long)-1。
    - 前端拿到这个异常大数后会被兜底逻辑显示成 0 B。

    所以这里按 MySQL 实际字段类型做兼容：
    - INT/SHORT/LONG 等走 as_int()
    - BIGINT/LONGLONG 走 as_ulonglong()
    - 其它异常情况返回 0
*/
static long long mysql_cell_to_file_size(const MySQLCell& cell)
{
    /*
        tbl_file.size 当前是普通整数类型时，会走这里。
    */
    if (cell.is_int()) {
        return cell.as_int();
    }

    /*
        如果后续把 size 改成 BIGINT，会走这里。
    */
    if (cell.is_ulonglong()) {
        return static_cast<long long>(cell.as_ulonglong());
    }

    /*
        类型不符合预期时返回 0，避免前端显示异常。
    */
    return 0;
}

/*
    FileMetaService 是第四期拆出来的文件元数据微服务。

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

        /*
            user_id 非法时直接返回参数错误。
        */
        if (user_id <= 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        /*
            查询当前用户的文件列表。
            这里沿用第三期的排序规则：最近更新的文件排在前面。
        */
        string sql =
            "SELECT id, filename, size, created_at, last_update "
            "FROM tbl_file "
            "WHERE uid=" + to_string(user_id) + " "
            "ORDER BY last_update DESC, id DESC;";

        /*
            db_error 表示 SELECT 是否异常。
        */
        bool db_error = false;

        /*
            执行查询。
        */
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            /*
                SELECT 成功时状态应该是 MYSQL_STATUS_GET_RESULT。
            */
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            /*
                row 保存每次 fetch_row() 读出的一行文件记录。
            */
            vector<MySQLCell> row;

            /*
                文件列表为空不是错误。
                如果没有文件，while 一次也不会进入，response->files 仍然是空 repeated 字段。
            */
            while (cursor.fetch_row(row)) {
                /*
                    add_files() 会在 repeated files 后面追加一个 FileInfo，
                    并返回这个新元素的可写指针。
                */
                cloud::disk::FileInfo* file = response->add_files();

                /*
                    按 SELECT 字段顺序写入 FileInfo。
                */
                file->set_file_id(row[0].as_int());
                file->set_filename(row[1].as_string());
                file->set_size(mysql_cell_to_file_size(row[2]));
                file->set_created_at(row[3].as_string());
                file->set_updated_at(row[4].as_string());
            }
        });

        /*
            查询失败时返回内部错误。
        */
        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        /*
            成功时 code=0。
        */
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
        /*
            从请求中取出字段。
        */
        int user_id = request->user_id();
        string filename = request->filename();
        string hashcode = request->hashcode();
        long long size = request->size();

        /*
            做基础参数校验。
        */
        if (user_id <= 0 || filename.empty() || hashcode.empty() || size < 0) {
            set_result(response->mutable_result(), 400, "请求格式有误");
            return;
        }

        /*
            拼接 INSERT SQL。
        */
        string sql =
            "INSERT INTO tbl_file (uid, filename, hashcode, size) VALUES (" +
            to_string(user_id) + ", '" +
            escape_sql(filename) + "', '" +
            escape_sql(hashcode) + "', " +
            to_string(size) + ");";

        /*
            file_id 保存新插入文件记录的 id。
        */
        int file_id = 0;

        /*
            cursor_status 保存 INSERT 结果状态。
        */
        int cursor_status = MYSQL_STATUS_ERROR;

        /*
            执行 INSERT。
        */
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            /*
                INSERT 成功时状态应该是 MYSQL_STATUS_OK。
            */
            cursor_status = cursor.get_cursor_status();

            /*
                获取自增 id。
            */
            file_id = static_cast<int>(cursor.get_insert_id());
        });

        /*
            文件元数据写入失败时，网关会删除临时文件并返回 500。
        */
        if (!query_ok || cursor_status != MYSQL_STATUS_OK) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        /*
            设置成功结果。
        */
        set_result(response->mutable_result(), 0, "上传成功");

        /*
            返回文件 id 和文件名。
        */
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
        /*
            取出用户 id 和文件 id。
        */
        int user_id = request->user_id();
        int file_id = request->file_id();

        /*
            基础参数校验。
        */
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

        /*
            found 表示是否找到文件记录。
        */
        bool found = false;

        /*
            db_error 表示 SELECT 是否异常。
        */
        bool db_error = false;

        /*
            保存查询出的文件名和 hashcode。
        */
        string filename;
        string hashcode;

        /*
            执行查询。
        */
        bool query_ok = run_mysql_query(sql, [&](MySQLResultCursor& cursor) {
            /*
                SELECT 成功时状态应该是 MYSQL_STATUS_GET_RESULT。
            */
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT) {
                db_error = true;
                return;
            }

            /*
                row 保存查询结果。
            */
            vector<MySQLCell> row;

            /*
                没有行表示文件不存在或不属于当前用户。
            */
            if (!cursor.fetch_row(row)) {
                found = false;
                return;
            }

            /*
                找到了文件记录。
            */
            found = true;

            /*
                拷贝 filename/hashcode。
            */
            filename = row[0].as_string();
            hashcode = row[1].as_string();
        });

        /*
            查询失败返回 500。
        */
        if (!query_ok || db_error) {
            set_result(response->mutable_result(), 500, "内部服务器错误");
            return;
        }

        /*
            文件不存在返回 404。
        */
        if (!found) {
            set_result(response->mutable_result(), 404, "文件不存在");
            return;
        }

        /*
            成功。
        */
        set_result(response->mutable_result(), 0, "获取下载信息成功");

        /*
            返回下载所需元数据。
        */
        response->set_filename(filename);
        response->set_hashcode(hashcode);
    }
};

/*
    主线程等待器。
*/
static WFFacilities::WaitGroup wait_group(1);

/*
    Ctrl+C 时通知主线程退出。
*/
static void sig_handler(int)
{
    /*
        唤醒 main() 中的 wait_group.wait()。
    */
    wait_group.done();
}

int main()
{
    /*
        初始化 protobuf。
    */
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    /*
        注册 Ctrl+C 信号。
    */
    signal(SIGINT, sig_handler);

    /*
        读取端口。
        如果 FILEMETA_SERVICE_PORT 没有配置，默认监听 9003。
    */
    unsigned short port = get_env_port("FILEMETA_SERVICE_PORT", 9003);

    /*
        创建 srpc server。
    */
    srpc::SRPCServer server;

    /*
        创建文件元数据服务实现对象。
    */
    FileMetaServiceImpl service;

    /*
        注册 FileMetaService。
    */
    server.add_service(&service);

    /*
        启动服务。
    */
    if (server.start(port) == 0) {
        cout << "[FileMetaService] listening on " << port << endl;

        /*
            等待 Ctrl+C。
        */
        wait_group.wait();

        /*
            停止服务。
        */
        server.stop();
    } else {
        /*
            启动失败时打印端口，方便排查是否被占用。
        */
        cerr << "[FileMetaService] start FAILED on port " << port << endl;
    }

    /*
        释放 protobuf 资源。
    */
    google::protobuf::ShutdownProtobufLibrary();

    /*
        正常结束。
    */
    return 0;
}
