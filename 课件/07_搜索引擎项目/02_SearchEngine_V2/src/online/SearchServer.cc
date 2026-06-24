#include "../../include/online/SearchServer.h"

#include "../../include/online/ProtocolCodec.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include <functional>
#include <nlohmann/json.hpp>

using namespace std::placeholders;

namespace
{
constexpr uint8_t kKeywordRequest = 1;
constexpr uint8_t kWebRequest = 2;
constexpr uint8_t kErrorResponse = 100;
}

SearchServer::SearchServer(muduo::net::EventLoop* loop,
                           const muduo::net::InetAddress& listenAddr,
                           KeywordRecommender& recommender,
                           WebSearcher& searcher,
                           int ioThreads,
                           uint32_t maxMessageSize,
                           int keywordTopK,
                           int webTopK)
    : server_(loop, listenAddr, "SearchServer")
    , recommender_(recommender)
    , searcher_(searcher)
    , maxMessageSize_(maxMessageSize)
    , keywordTopK_(keywordTopK)
    , webTopK_(webTopK)
{
    server_.setConnectionCallback(std::bind(&SearchServer::on_connection, this, _1));
    server_.setMessageCallback(std::bind(&SearchServer::on_message, this, _1, _2, _3));
    server_.setThreadNum(ioThreads);
}

void SearchServer::start()
{
    server_.start();
}

void SearchServer::on_connection(const muduo::net::TcpConnectionPtr& conn)
{
    if (conn->connected()) {
        LOG_INFO << "client connected: " << conn->peerAddress().toIpPort();
    } else {
        LOG_INFO << "client disconnected: " << conn->peerAddress().toIpPort();
    }
}

void SearchServer::on_message(const muduo::net::TcpConnectionPtr& conn,
                              muduo::net::Buffer* buffer,
                              muduo::Timestamp)
{
    try {
        Request request;
        while (ProtocolCodec::try_decode(buffer, request, maxMessageSize_)) {
            uint8_t responseType = request.type;
            std::string response = handle_request(request.type, request.value, responseType);
            conn->send(ProtocolCodec::encode(responseType, response));
        }
    } catch (const std::exception& ex) {
        conn->send(ProtocolCodec::encode(kErrorResponse, make_error(ex.what())));
    }
}

std::string SearchServer::handle_request(uint8_t type, const std::string& value, uint8_t& responseType)
{
    nlohmann::json request = nlohmann::json::parse(value);
    std::string query = request.value("query", "");
    if (query.empty()) {
        responseType = kErrorResponse;
        return make_error("query is empty");
    }

    if (type == kKeywordRequest) {
        std::string lang = request.value("lang", "");
        int topK = request.value("topk", keywordTopK_);
        return recommender_.recommend_json(query, lang, topK);
    }

    if (type == kWebRequest) {
        int topK = request.value("topk", webTopK_);
        return searcher_.search_json(query, topK);
    }

    responseType = kErrorResponse;
    return make_error("unsupported request type");
}

std::string SearchServer::make_error(const std::string& message) const
{
    nlohmann::json response;
    response["error"] = "invalid request";
    response["message"] = message;
    return response.dump();
}
