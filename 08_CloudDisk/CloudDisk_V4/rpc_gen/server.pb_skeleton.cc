#include "cloud_disk.srpc.h"
#include "workflow/WFFacilities.h"

using namespace srpc;

static WFFacilities::WaitGroup wait_group(1);

void sig_handler(int signo)
{
	wait_group.done();
}

class AuthServiceServiceImpl : public ::cloud::disk::AuthService::Service
{
public:

	void Register(::cloud::disk::RegisterRequest *request, ::cloud::disk::RegisterResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}

	void Login(::cloud::disk::LoginRequest *request, ::cloud::disk::LoginResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}

	void VerifyToken(::cloud::disk::VerifyTokenRequest *request, ::cloud::disk::VerifyTokenResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}
};
class UserServiceServiceImpl : public ::cloud::disk::UserService::Service
{
public:

	void GetUserProfile(::cloud::disk::GetUserProfileRequest *request, ::cloud::disk::GetUserProfileResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}
};
class FileMetaServiceServiceImpl : public ::cloud::disk::FileMetaService::Service
{
public:

	void ListFiles(::cloud::disk::ListFilesRequest *request, ::cloud::disk::ListFilesResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}

	void CreateFile(::cloud::disk::CreateFileRequest *request, ::cloud::disk::CreateFileResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}

	void GetFileForDownload(::cloud::disk::GetFileForDownloadRequest *request, ::cloud::disk::GetFileForDownloadResponse *response, srpc::RPCContext *ctx) override
	{
		// TODO: fill server logic here
	}
};

int main()
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	unsigned short port = 1412;
	SRPCServer server;

	AuthServiceServiceImpl authservice_impl;
	server.add_service(&authservice_impl);

	UserServiceServiceImpl userservice_impl;
	server.add_service(&userservice_impl);

	FileMetaServiceServiceImpl filemetaservice_impl;
	server.add_service(&filemetaservice_impl);

	server.start(port);
	wait_group.wait();
	server.stop();
	google::protobuf::ShutdownProtobufLibrary();
	return 0;
}
