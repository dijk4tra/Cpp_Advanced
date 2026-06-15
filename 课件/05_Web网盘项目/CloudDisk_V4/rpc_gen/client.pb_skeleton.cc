#include "cloud_disk.srpc.h"
#include "workflow/WFFacilities.h"

using namespace srpc;

static WFFacilities::WaitGroup wait_group(1);

void sig_handler(int signo)
{
	wait_group.done();
}

static void register_done(::cloud::disk::RegisterResponse *response, srpc::RPCContext *context)
{
}

static void login_done(::cloud::disk::LoginResponse *response, srpc::RPCContext *context)
{
}

static void verifytoken_done(::cloud::disk::VerifyTokenResponse *response, srpc::RPCContext *context)
{
}

static void getuserprofile_done(::cloud::disk::GetUserProfileResponse *response, srpc::RPCContext *context)
{
}

static void listfiles_done(::cloud::disk::ListFilesResponse *response, srpc::RPCContext *context)
{
}

static void createfile_done(::cloud::disk::CreateFileResponse *response, srpc::RPCContext *context)
{
}

static void getfilefordownload_done(::cloud::disk::GetFileForDownloadResponse *response, srpc::RPCContext *context)
{
}

int main()
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	const char *ip = "127.0.0.1";
	unsigned short port = 1412;

	::cloud::disk::AuthService::SRPCClient client(ip, port);

	// example for RPC method call
	::cloud::disk::RegisterRequest register_req;
	//register_req.set_message("Hello, srpc!");
	client.Register(&register_req, register_done);

	::cloud::disk::UserService::SRPCClient client1(ip, port);

	// example for RPC method call
	::cloud::disk::GetUserProfileRequest getuserprofile_req;
	//getuserprofile_req.set_message("Hello, srpc!");
	client1.GetUserProfile(&getuserprofile_req, getuserprofile_done);

	::cloud::disk::FileMetaService::SRPCClient client2(ip, port);

	// example for RPC method call
	::cloud::disk::ListFilesRequest listfiles_req;
	//listfiles_req.set_message("Hello, srpc!");
	client2.ListFiles(&listfiles_req, listfiles_done);

	wait_group.wait();
	google::protobuf::ShutdownProtobufLibrary();
	return 0;
}
