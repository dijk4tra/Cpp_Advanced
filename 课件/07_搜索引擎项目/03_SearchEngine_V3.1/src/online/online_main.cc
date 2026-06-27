#include "../../include/common/Config.h"
#include "../../include/cache/CachedSearchService.h"
#include "../../include/cache/RedisCache.h"
#include "../../include/cache/ShardedLruCache.h"
#include "../../include/cache/ShardedWTinyLfuCache.h"
#include "../../include/cache/TwoLevelCache.h"
#include "../../include/online/KeywordRecommender.h"
#include "../../include/online/SearchServer.h"
#include "../../include/online/WebHttpServer.h"
#include "../../include/online/WebSearcher.h"

#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/base/Logging.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace
{
// 配置读取辅助函数放在匿名命名空间中，表示它们只属于当前程序入口文件。

/**
 * @brief 读取字符串配置项，不存在时返回默认值。
 *
 * Config::get 对缺失 key 会抛异常。在线服务部分配置允许有默认值，因此在入口
 * 处用该函数统一兜底。
 */
std::string get_or(const Config& config, const std::string& key, const std::string& fallback)
{
    try {
        // Config::get 返回内部字符串的 const 引用；这里按值返回，调用方拿到独立副本。
        return config.get(key);
    } catch (const std::exception&) {
        // 缺少可选配置项时不终止程序，使用调用方提供的默认值。
        return fallback;
    }
}

/**
 * @brief 读取整数配置项，不存在或格式非法时返回默认值。
 */
int get_int_or(const Config& config, const std::string& key, int fallback)
{
    try {
        // stoi 将字符串配置转成 int。配置不存在或不是合法整数都会进入 catch。
        return std::stoi(config.get(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

/**
 * @brief 读取浮点配置项，不存在或格式非法时返回默认值。
 */
double get_double_or(const Config& config, const std::string& key, double fallback)
{
    try {
        return std::stod(config.get(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

void configure_muduo_log_level(const std::string& level)
{
    if (level == "TRACE") {
        muduo::Logger::setLogLevel(muduo::Logger::TRACE);
    } else if (level == "DEBUG") {
        muduo::Logger::setLogLevel(muduo::Logger::DEBUG);
    } else if (level == "INFO") {
        muduo::Logger::setLogLevel(muduo::Logger::INFO);
    } else if (level == "ERROR") {
        muduo::Logger::setLogLevel(muduo::Logger::ERROR);
    } else {
        // 线上默认 WARN，避免 muduo TcpServer 为每次建连/断连写 INFO 日志。
        muduo::Logger::setLogLevel(muduo::Logger::WARN);
    }
}
}

/**
 * @brief 搜索引擎第三期在线服务入口。
 *
 * 程序启动后先加载一期生成的词典、索引和偏移库，再按配置组装 L1/L2 缓存，
 * 最后启动 muduo TLV 服务和 HTTP 服务。网页库正文在第三期改为按需读取，
 * 热点文档和摘要由缓存保存。
 */
int main()
{
    try {
        // 统一从项目根目录下的 conf/config.conf 读取路径、端口和线程数。
        // Config 是栈对象，main 结束时自动析构。
        Config config("conf/config.conf");
        configure_muduo_log_level(get_or(config, "muduo_log_level", "WARN"));

        std::cout << "========== SearchEngine V3 Online Server ==========" << std::endl;

        // 关键字推荐依赖一期离线生成的中英文词典和字符索引。
        KeywordRecommender recommender;
        // config.get() 查不到必需路径时会抛异常，直接进入 catch 并退出程序。
        recommender.load(config.get("cn_dict"),
                         config.get("cn_dict_index"),
                         config.get("en_dict"),
                         config.get("en_dict_index"));

        // 网页搜索依赖网页库、偏移库、BM25 倒排索引、文档长度统计和中文停用词。
        // PageLibrary::load 在第三期只加载偏移库并记录 pages.dat 路径，
        // 不再把网页正文全量读入内存。
        WebSearcher searcher;
        searcher.load(config.get("pages"),
                      config.get("offsets"),
                      config.get("invert_index"),
                      config.get("bm25_doc_stats"),
                      config.get("cn_stop_words"));
        searcher.set_bm25_parameters(get_double_or(config, "bm25_k1", 1.5),
                                     get_double_or(config, "bm25_b", 0.75));

        std::string ip = get_or(config, "server_ip", "0.0.0.0");
        int port = get_int_or(config, "server_port", 8888);
        int httpPort = get_int_or(config, "http_port", 18888);
        int ioThreads = get_int_or(config, "io_threads", 4);
        int httpThreads = get_int_or(config, "http_threads", 8);
        httpThreads = httpThreads > 0 ? httpThreads : 8;
        int httpMaxRequestSize =
            get_int_or(config, "http_max_request_size", 1024 * 1024);
        httpMaxRequestSize = httpMaxRequestSize > 0 ? httpMaxRequestSize : 1024 * 1024;
        int keywordTopK = get_int_or(config, "keyword_topk", 10);
        int webTopK = get_int_or(config, "web_topk", 33);
        int maxMessageSize = get_int_or(config, "max_message_size", 1024 * 1024);
        std::string wwwRoot = get_or(config, "www_root", "www");
        int abstractLength = get_int_or(config, "abstract_length", 150);
        // 摘要长度属于 WebSearcher 的运行参数，加载索引后再设置即可。
        searcher.set_abstract_length(abstractLength);

        // 缓存相关配置分为三类：
        // 1. 总开关和 L1/L2 开关；
        // 2. 容量、分片数、TTL 等缓存策略；
        // 3. Redis 连接参数。
        // get_int_or 允许旧配置文件不包含第三期新增字段，缺失时使用默认值。
        int cacheEnabled = get_int_or(config, "cache_enabled", 1);
        int l1CacheEnabled = get_int_or(config, "l1_cache_enabled", 1);
        int l1CacheCapacity = get_int_or(config, "l1_cache_capacity", 4096);
        int l1CacheShards = get_int_or(config, "l1_cache_shards", 32);
        std::string l1CachePolicy = get_or(config, "l1_cache_policy", "wtinylfu");
        int l1WindowPercent = get_int_or(config, "l1_wtinylfu_window_percent", 1);
        int l1ProtectedPercent = get_int_or(config, "l1_wtinylfu_protected_percent", 80);
        int l1FrequencySampleMultiplier =
            get_int_or(config, "l1_wtinylfu_frequency_sample_multiplier", 10);
        int l1CacheTtl = get_int_or(config, "l1_cache_ttl_seconds", 600);
        int l1EmptyTtl = get_int_or(config, "l1_empty_ttl_seconds", 60);
        int documentCacheTtl = get_int_or(config, "document_cache_ttl_seconds", 600);
        int abstractCacheTtl = get_int_or(config, "abstract_cache_ttl_seconds", 600);
        int cacheStatsInterval = get_int_or(config, "cache_stats_log_interval", 100);
        int cacheTtlJitter = get_int_or(config, "cache_ttl_jitter_seconds", 30);
        std::string cacheVersion = get_or(config, "cache_version", "search_engine_v3_001");
        int redisCacheEnabled = get_int_or(config, "redis_cache_enabled", 0);
        std::string redisHost = get_or(config, "redis_host", "127.0.0.1");
        int redisPort = get_int_or(config, "redis_port", 6379);
        int redisDb = get_int_or(config, "redis_db", 0);
        int redisConnectTimeout = get_int_or(config, "redis_connect_timeout_ms", 20);
        int redisCommandTimeout = get_int_or(config, "redis_command_timeout_ms", 20);
        int redisPoolSize = get_int_or(config, "redis_pool_size", 16);
        int redisPoolWaitTimeout = get_int_or(config, "redis_pool_wait_timeout_ms", 20);
        redisPoolSize = redisPoolSize > 0 ? redisPoolSize : 16;
        redisPoolWaitTimeout = redisPoolWaitTimeout > 0 ? redisPoolWaitTimeout : 20;
        int redisBackfillTtl = get_int_or(config, "redis_l1_backfill_ttl_seconds", l1CacheTtl);

        // unique_ptr 负责缓存对象生命周期。后面传给业务模块的是裸指针 Cache*，
        // 但实际对象一直由这些 unique_ptr 持有，直到 main 退出。
        std::unique_ptr<Cache> l1Cache;
        std::unique_ptr<RedisCache> redisCache;
        std::unique_ptr<TwoLevelCache> twoLevelCache;
        // cache 始终指向“最终对外使用的缓存”：可能是 L1、Redis、TwoLevelCache，
        // 也可能保持 nullptr 表示完全关闭缓存。
        Cache* cache = nullptr;
        if (cacheEnabled != 0 && l1CacheEnabled != 0) {
            // 容量和分片数不能为 0。配置非法时回退默认值，避免后续取模或淘汰逻辑异常。
            l1CacheCapacity = l1CacheCapacity > 0 ? l1CacheCapacity : 4096;
            l1CacheShards = l1CacheShards > 0 ? l1CacheShards : 32;
            // 默认使用第十阶段的完整 W-TinyLFU。保留 lru 选项便于回归测试、
            // 对比命中率，并在新策略出现异常时快速回退。
            if (l1CachePolicy == "lru") {
                l1Cache = std::make_unique<ShardedLruCache>(
                    static_cast<std::size_t>(l1CacheCapacity),
                    static_cast<std::size_t>(l1CacheShards));
            } else {
                l1CachePolicy = "wtinylfu";
                // 入口先完成范围校验，使启动日志中的值与缓存实际采用的值一致。
                l1WindowPercent = std::clamp(l1WindowPercent, 1, 99);
                l1ProtectedPercent = std::clamp(l1ProtectedPercent, 1, 99);
                l1FrequencySampleMultiplier =
                    l1FrequencySampleMultiplier > 0 ? l1FrequencySampleMultiplier : 10;
                l1Cache = std::make_unique<ShardedWTinyLfuCache>(
                    static_cast<std::size_t>(l1CacheCapacity),
                    static_cast<std::size_t>(l1CacheShards),
                    static_cast<std::size_t>(l1WindowPercent),
                    static_cast<std::size_t>(l1ProtectedPercent),
                    static_cast<std::size_t>(l1FrequencySampleMultiplier));
            }
            // 如果后面没有 Redis 或 TwoLevelCache，这个指针就是最终缓存。
            cache = l1Cache.get();
            std::cout << "[Cache] L1 enabled, policy=" << l1CachePolicy
                      << ", capacity=" << l1CacheCapacity
                      << ", shards=" << l1CacheShards
                      << ", ttl=" << l1CacheTtl
                      << ", empty_ttl=" << l1EmptyTtl;
            if (l1CachePolicy == "wtinylfu") {
                std::cout << ", window_percent=" << l1WindowPercent
                          << ", protected_percent=" << l1ProtectedPercent
                          << ", frequency_sample_multiplier="
                          << l1FrequencySampleMultiplier;
            }
            std::cout << std::endl;
        } else {
            std::cout << "[Cache] L1 disabled" << std::endl;
        }

        if (cacheEnabled != 0 && redisCacheEnabled != 0) {
            // RedisCache 使用惰性持久连接池。构造时不强制连接 Redis，首次访问时
            // 才建连；Redis 异常会在 get/put/erase 时降级为未命中或写入失败。
            redisCache = std::make_unique<RedisCache>(redisHost,
                                                      redisPort,
                                                      redisDb,
                                                      redisConnectTimeout,
                                                      redisCommandTimeout,
                                                      redisPoolSize,
                                                      redisPoolWaitTimeout);
            std::cout << "[Cache] Redis L2 enabled, host=" << redisHost
                      << ", port=" << redisPort
                      << ", db=" << redisDb
                      << ", pool_size=" << redisPoolSize
                      << std::endl;
        }

        if (cacheEnabled != 0) {
            if (l1Cache != nullptr && redisCache != nullptr) {
                // 同时启用 L1 和 Redis 时，用 TwoLevelCache 组合成统一 Cache 接口。
                // 业务层不需要知道内部是“先查本地再查 Redis”的二级结构。
                twoLevelCache = std::make_unique<TwoLevelCache>(l1Cache.get(),
                                                                redisCache.get(),
                                                                redisBackfillTtl);
                cache = twoLevelCache.get();
            } else if (redisCache != nullptr) {
                // 只启用 Redis 时直接把 Redis 当作统一缓存。
                cache = redisCache.get();
            } else if (l1Cache != nullptr) {
                // 只启用 L1 时直接使用本地缓存。
                cache = l1Cache.get();
            } else {
                std::cout << "[Cache] disabled" << std::endl;
            }
        } else {
            std::cout << "[Cache] disabled" << std::endl;
        }

        // 细粒度缓存由 WebSearcher 内部使用：
        // 1. document cache：docId -> 文档展示信息，避免频繁从 pages.dat 按需读取；
        // 2. abstract cache：docId + query keywords -> 动态摘要，避免重复生成片段。
        searcher.set_detail_cache(cache,
                                  cacheVersion,
                                  documentCacheTtl,
                                  abstractCacheTtl);

        // CachedSearchService 是更外层的业务结果缓存：
        // 1. suggest(query, lang, topK) -> 推荐 JSON；
        // 2. search(query, topK) -> 搜索 JSON。
        // 它还负责 singleflight、空结果缓存、TTL 抖动和命中率统计。
        CachedSearchService cachedService(recommender,
                                          searcher,
                                          cache,
                                          cacheVersion,
                                          l1CacheTtl,
                                          l1EmptyTtl,
                                          abstractLength,
                                          cacheStatsInterval,
                                          cacheTtlJitter);

        // 两个服务器共用同一个 EventLoop：
        // 1. SearchServer 监听 TLV 端口，供课程协议客户端使用；
        // 2. WebHttpServer 监听 HTTP 端口，供浏览器页面使用。
        muduo::net::EventLoop loop;
        // InetAddress 的端口类型是 uint16_t，因此把 int 端口显式转换为 16 位无符号整数。
        muduo::net::InetAddress listenAddr(ip, static_cast<uint16_t>(port));
        SearchServer server(&loop,
                            listenAddr,
                            cachedService,
                            ioThreads,
                            // maxMessageSize 配置读出是 int，ProtocolCodec 需要 uint32_t。
                            static_cast<uint32_t>(maxMessageSize),
                            keywordTopK,
                            webTopK);

        muduo::net::InetAddress httpListenAddr(ip, static_cast<uint16_t>(httpPort));
        WebHttpServer httpServer(&loop,
                                 httpListenAddr,
                                 cachedService,
                                 wwwRoot,
                                 httpThreads,
                                 keywordTopK,
                                 webTopK,
                                 static_cast<std::size_t>(httpMaxRequestSize));

        server.start();
        httpServer.start();
        std::cout << "[Online] TLV listen on " << ip << ":" << port << std::endl;
        std::cout << "[Online] Web listen on http://" << ip << ":" << httpPort << std::endl;
        // loop() 进入 muduo 事件循环，之后由回调函数处理连接和请求。
        loop.loop();
        return 0;
    } catch (const std::exception& ex) {
        // 启动阶段任何异常都在这里统一打印，返回非 0 表示程序启动失败。
        std::cerr << "[Error] " << ex.what() << std::endl;
        return 1;
    }
}
