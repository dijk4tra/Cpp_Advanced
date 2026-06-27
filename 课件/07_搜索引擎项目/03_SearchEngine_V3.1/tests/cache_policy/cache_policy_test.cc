#include "../../include/cache/ShardedWTinyLfuCache.h"
#include "../../include/cache/TinyLfuFrequencySketch.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
void test_frequency_sketch()
{
    const std::uint64_t hash = 0x123456789abcdef0ULL;
    TinyLfuFrequencySketch sketch(32, 1000);
    const std::size_t memoryBytes = sketch.memory_usage_bytes();

    assert(sketch.estimate(hash) == 0);
    sketch.increment(hash);
    // 第一次访问只进入 Doorkeeper，不写 Count-Min Sketch。
    assert(sketch.estimate(hash) == 1);
    sketch.increment(hash);
    assert(sketch.estimate(hash) == 2);

    // 4-bit counter 最大为 15，再加 Doorkeeper 的 1，估算值最大为 16。
    for (int i = 0; i < 100; ++i) {
        sketch.increment(hash);
    }
    assert(sketch.estimate(hash) == 16);
    assert(sketch.memory_usage_bytes() == memoryBytes);

    TinyLfuFrequencySketch agingSketch(8, 4);
    for (std::uint64_t i = 0; i < 4; ++i) {
        agingSketch.increment(i * 17 + 1);
    }
    assert(agingSketch.reset_count() == 1);
}

void test_scan_resistance()
{
    // 单分片和较大的 Window 比例让测试的分段容量固定且容易推导：
    // capacity=8，Window=2，Main=6。
    ShardedWTinyLfuCache cache(8, 1, 25, 80, 100);
    std::string value;

    cache.put("hot", "hot-value", 0);
    for (int i = 0; i < 20; ++i) {
        assert(cache.get("hot", value));
    }

    // 填满 Window/Main，并让 hot 从 Window 进入 Main。
    for (int i = 0; i < 8; ++i) {
        const std::string key = "warm-" + std::to_string(i);
        cache.get(key, value);
        cache.put(key, key, 0);
    }
    assert(cache.get("hot", value));

    // 一次性扫描 key 的频率均为 1，不应替换频率明显更高的 hot。
    for (int i = 0; i < 200; ++i) {
        const std::string key = "scan-" + std::to_string(i);
        assert(!cache.get(key, value));
        cache.put(key, key, 0);
    }
    assert(cache.get("hot", value));
    assert(value == "hot-value");
    assert(cache.stats().rejections > 0);
}

void test_ttl_and_erase()
{
    ShardedWTinyLfuCache cache(8, 2);
    std::string value;

    cache.put("ttl", "value", 1);
    assert(cache.get("ttl", value));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(!cache.get("ttl", value));
    assert(cache.stats().expirations > 0);

    cache.put("erase", "value", 0);
    cache.erase("erase");
    assert(!cache.get("erase", value));
}

void test_concurrent_access()
{
    ShardedWTinyLfuCache cache(256, 16);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([worker, &cache]() {
            std::string value;
            for (int i = 0; i < 2000; ++i) {
                const std::string key = "key-" + std::to_string((i + worker) % 128);
                if (!cache.get(key, value)) {
                    cache.put(key, key, 0);
                }
                if (i % 31 == 0) {
                    cache.erase("key-" + std::to_string((i * 7) % 128));
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto stats = cache.stats();
    assert(stats.hits + stats.misses > 0);
}

void test_frequency_aging_stats()
{
    ShardedWTinyLfuCache cache(8, 1, 25, 80, 2);
    std::string value;
    const std::size_t sketchBytes = cache.frequency_sketch_bytes();
    assert(sketchBytes > 0);

    for (int i = 0; i < 32; ++i) {
        cache.get("aging-" + std::to_string(i), value);
    }
    assert(cache.stats().frequencyAges >= 2);
    assert(cache.frequency_sketch_bytes() == sketchBytes);
}
}

int main()
{
    test_frequency_sketch();
    test_scan_resistance();
    test_ttl_and_erase();
    test_concurrent_access();
    test_frequency_aging_stats();
    std::cout << "cache_policy_test: PASS" << std::endl;
    return 0;
}
