// 使用 protoc 和 srpc_generator 生成代码
protoc --cpp_out=./ example.proto
srpc_generator protobuf example.proto ./

// 编译
g++ server.cc example.pb.cc -lsrpc -llz4 -lsnappy -lprotobuf -lworkflow -o server
g++ client.cc example.pb.cc -lsrpc -llz4 -lsnappy -lprotobuf -lworkflow -o client
