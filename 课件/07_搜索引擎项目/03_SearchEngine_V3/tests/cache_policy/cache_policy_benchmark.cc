#include "../../include/cache/Cache.h"
#include "../../include/cache/ShardedLruCache.h"
#include "../../include/cache/ShardedWTinyLfuCache.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options {
    std::string traceFile;
    std::vector<std::size_t> capacities{32, 100, 512, 2048};
    std::vector<std::size_t> shardCounts{1, 8, 32};
    std::size_t repeats = 2;
    std::size_t requests = 10000;
    std::uint32_t seed = 20260627;
    bool csv = false;
};

struct Scenario {
    std::string name;
    std::vector<std::string> trace;
};

struct Result {
    std::string scenario;
    std::string policy;
    std::size_t capacity = 0;
    std::size_t shards = 0;
    std::uint64_t requests = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    double elapsedSeconds = 0.0;
    std::vector<std::uint64_t> latencyNs;
    std::uint64_t admissions = 0;
    std::uint64_t rejections = 0;
    std::uint64_t frequencyAges = 0;

    double hit_rate() const
    {
        return requests == 0 ? 0.0
                             : static_cast<double>(hits) * 100.0 /
                                   static_cast<double>(requests);
    }

    double qps() const
    {
        return elapsedSeconds <= 0.0 ? 0.0
                                     : static_cast<double>(requests) / elapsedSeconds;
    }
};

void print_usage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --trace FILE          replay one query/key per line instead of built-in traces\n"
        << "  --capacities LIST     comma-separated capacities, default 32,100,512,2048\n"
        << "  --shards LIST         comma-separated shard counts, default 1,8,32\n"
        << "  --repeats N           fresh-cache replay count, default 2\n"
        << "  --requests N          requests per generated trace, default 10000\n"
        << "  --seed N              deterministic random seed\n"
        << "  --csv                 print machine-readable CSV\n"
        << "  --help                show this help\n";
}

std::size_t parse_size(const std::string& text, const std::string& name)
{
    std::size_t used = 0;
    unsigned long long value = std::stoull(text, &used);
    if (used != text.size() || value == 0) {
        throw std::runtime_error("invalid " + name + ": " + text);
    }
    return static_cast<std::size_t>(value);
}

std::vector<std::size_t> parse_size_list(const std::string& text,
                                         const std::string& name)
{
    std::vector<std::size_t> result;
    std::istringstream input(text);
    std::string item;
    while (std::getline(input, item, ',')) {
        result.push_back(parse_size(item, name));
    }
    if (result.empty()) {
        throw std::runtime_error(name + " list cannot be empty");
    }
    return result;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };

        if (argument == "--trace") {
            options.traceFile = next(argument);
        } else if (argument == "--capacities") {
            options.capacities = parse_size_list(next(argument), "capacity");
        } else if (argument == "--shards") {
            options.shardCounts = parse_size_list(next(argument), "shards");
        } else if (argument == "--repeats") {
            options.repeats = parse_size(next(argument), "repeats");
        } else if (argument == "--requests") {
            options.requests = parse_size(next(argument), "requests");
        } else if (argument == "--seed") {
            options.seed = static_cast<std::uint32_t>(parse_size(next(argument), "seed"));
        } else if (argument == "--csv") {
            options.csv = true;
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    return options;
}

std::vector<std::string> load_trace(const std::string& path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open trace: " + path);
    }

    std::vector<std::string> trace;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() != '#') {
            trace.push_back(line);
        }
    }
    if (trace.empty()) {
        throw std::runtime_error("trace contains no usable keys: " + path);
    }
    return trace;
}

std::vector<std::string> build_scan_trace()
{
    std::vector<std::string> trace;
    for (int round = 0; round < 10; ++round) {
        for (int hot = 0; hot < 20; ++hot) {
            trace.push_back("hot-" + std::to_string(hot));
        }
    }
    for (int round = 0; round < 20; ++round) {
        for (int cold = 0; cold < 200; ++cold) {
            trace.push_back("scan-" + std::to_string(round) + "-" +
                            std::to_string(cold));
        }
        for (int repeat = 0; repeat < 5; ++repeat) {
            for (int hot = 0; hot < 20; ++hot) {
                trace.push_back("hot-" + std::to_string(hot));
            }
        }
    }
    return trace;
}

std::vector<std::string> build_zipf_trace(std::size_t requests, std::uint32_t seed)
{
    constexpr std::size_t keyCount = 4096;
    std::vector<double> weights(keyCount);
    for (std::size_t rank = 0; rank < keyCount; ++rank) {
        weights[rank] = 1.0 / std::pow(static_cast<double>(rank + 1), 1.1);
    }
    std::mt19937 generator(seed);
    std::discrete_distribution<std::size_t> distribution(weights.begin(), weights.end());

    std::vector<std::string> trace;
    trace.reserve(requests);
    for (std::size_t i = 0; i < requests; ++i) {
        trace.push_back("zipf-" + std::to_string(distribution(generator)));
    }
    return trace;
}

std::vector<std::string> build_uniform_trace(std::size_t requests, std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> distribution(0, 4095);
    std::vector<std::string> trace;
    trace.reserve(requests);
    for (std::size_t i = 0; i < requests; ++i) {
        trace.push_back("uniform-" + std::to_string(distribution(generator)));
    }
    return trace;
}

std::vector<std::string> build_hot_shift_trace(std::size_t requests, std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> hotDistribution(0, 63);
    std::uniform_int_distribution<int> percent(0, 99);
    std::vector<std::string> trace;
    trace.reserve(requests);

    std::size_t coldId = 0;
    for (std::size_t i = 0; i < requests; ++i) {
        if (percent(generator) < 90) {
            const int phaseOffset = i < requests / 2 ? 0 : 64;
            trace.push_back("shift-hot-" +
                            std::to_string(phaseOffset + hotDistribution(generator)));
        } else {
            trace.push_back("shift-cold-" + std::to_string(coldId++));
        }
    }
    return trace;
}

std::vector<std::string> build_burst_trace(std::size_t requests, std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> hotDistribution(0, 19);
    std::uniform_int_distribution<int> percent(0, 99);
    std::vector<std::string> trace;
    trace.reserve(requests);

    std::size_t coldId = 0;
    for (std::size_t i = 0; i < requests; ++i) {
        if (percent(generator) < 80) {
            trace.push_back("burst-hot-" + std::to_string(hotDistribution(generator)));
        } else {
            trace.push_back("burst-once-" + std::to_string(coldId++));
        }
    }
    return trace;
}

std::vector<std::string> build_cyclic_trace(std::size_t requests)
{
    constexpr std::size_t workingSet = 640;
    std::vector<std::string> trace;
    trace.reserve(requests);
    for (std::size_t i = 0; i < requests; ++i) {
        trace.push_back("cycle-" + std::to_string(i % workingSet));
    }
    return trace;
}

std::vector<Scenario> build_scenarios(const Options& options)
{
    if (!options.traceFile.empty()) {
        return {{"query_log", load_trace(options.traceFile)}};
    }
    return {
        {"scan_resistance", build_scan_trace()},
        {"zipf_1.1", build_zipf_trace(options.requests, options.seed)},
        {"uniform", build_uniform_trace(options.requests, options.seed + 1)},
        {"hot_shift", build_hot_shift_trace(options.requests, options.seed + 2)},
        {"burst_long_tail", build_burst_trace(options.requests, options.seed + 3)},
        {"cyclic_working_set", build_cyclic_trace(options.requests)},
    };
}

void replay(Cache& cache, const std::vector<std::string>& trace, Result& result)
{
    std::string value;
    for (const std::string& key : trace) {
        const auto started = Clock::now();
        if (cache.get(key, value)) {
            ++result.hits;
        } else {
            ++result.misses;
            cache.put(key, key, 0);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started);
        result.latencyNs.push_back(static_cast<std::uint64_t>(elapsed.count()));
        ++result.requests;
    }
}

Result run_lru(const Scenario& scenario,
               std::size_t capacity,
               std::size_t shards,
               std::size_t repeats)
{
    Result result;
    result.scenario = scenario.name;
    result.policy = "lru";
    result.capacity = capacity;
    result.shards = std::min(shards, capacity);
    result.latencyNs.reserve(scenario.trace.size() * repeats);
    const auto started = Clock::now();
    for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        ShardedLruCache cache(capacity, result.shards);
        replay(cache, scenario.trace, result);
    }
    result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
    return result;
}

Result run_wtinylfu(const Scenario& scenario,
                    std::size_t capacity,
                    std::size_t shards,
                    std::size_t repeats)
{
    Result result;
    result.scenario = scenario.name;
    result.policy = "wtinylfu";
    result.capacity = capacity;
    result.shards = std::min(shards, capacity);
    result.latencyNs.reserve(scenario.trace.size() * repeats);
    const auto started = Clock::now();
    for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        ShardedWTinyLfuCache cache(capacity, result.shards, 1, 80, 10);
        replay(cache, scenario.trace, result);
        const auto stats = cache.stats();
        result.admissions += stats.admissions;
        result.rejections += stats.rejections;
        result.frequencyAges += stats.frequencyAges;
    }
    result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
    return result;
}

double percentile_us(const Result& result, double percentile)
{
    if (result.latencyNs.empty()) {
        return 0.0;
    }
    std::vector<std::uint64_t> sorted = result.latencyNs;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(percentile / 100.0 * static_cast<double>(sorted.size())));
    const std::size_t index = std::min(sorted.size() - 1, std::max<std::size_t>(1, rank) - 1);
    return static_cast<double>(sorted[index]) / 1000.0;
}

void print_header(bool csv)
{
    if (csv) {
        std::cout << "scenario,capacity,shards,policy,requests,hits,misses,hit_rate_pct,"
                     "p50_us,p95_us,p99_us,qps,admissions,rejections,frequency_ages\n";
        return;
    }
    std::cout << std::left
              << std::setw(20) << "scenario"
              << std::right
              << std::setw(7) << "cap"
              << std::setw(7) << "shard"
              << std::setw(11) << "policy"
              << std::setw(10) << "hit%"
              << std::setw(10) << "p50us"
              << std::setw(10) << "p95us"
              << std::setw(10) << "p99us"
              << std::setw(13) << "QPS"
              << std::setw(11) << "admit"
              << std::setw(11) << "reject"
              << std::setw(9) << "ages"
              << '\n';
}

void print_result(const Result& result, bool csv)
{
    const double p50 = percentile_us(result, 50.0);
    const double p95 = percentile_us(result, 95.0);
    const double p99 = percentile_us(result, 99.0);
    if (csv) {
        std::cout << result.scenario << ',' << result.capacity << ',' << result.shards << ','
                  << result.policy << ',' << result.requests << ',' << result.hits << ','
                  << result.misses << ',' << std::fixed << std::setprecision(4)
                  << result.hit_rate() << ',' << p50 << ',' << p95 << ',' << p99 << ','
                  << std::setprecision(2) << result.qps() << ',' << result.admissions << ','
                  << result.rejections << ',' << result.frequencyAges << '\n';
        return;
    }
    std::cout << std::left << std::setw(20) << result.scenario
              << std::right << std::setw(7) << result.capacity
              << std::setw(7) << result.shards
              << std::setw(11) << result.policy
              << std::fixed << std::setprecision(2)
              << std::setw(10) << result.hit_rate()
              << std::setw(10) << p50
              << std::setw(10) << p95
              << std::setw(10) << p99
              << std::setw(13) << result.qps()
              << std::setw(11) << result.admissions
              << std::setw(11) << result.rejections
              << std::setw(9) << result.frequencyAges
              << '\n';
}
}

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        const std::vector<Scenario> scenarios = build_scenarios(options);
        print_header(options.csv);

        bool checkedScanRegression = false;
        for (const Scenario& scenario : scenarios) {
            for (std::size_t capacity : options.capacities) {
                for (std::size_t shards : options.shardCounts) {
                    const Result lru = run_lru(
                        scenario, capacity, shards, options.repeats);
                    const Result tiny = run_wtinylfu(
                        scenario, capacity, shards, options.repeats);
                    print_result(lru, options.csv);
                    print_result(tiny, options.csv);

                    assert(lru.hits + lru.misses == lru.requests);
                    assert(tiny.hits + tiny.misses == tiny.requests);
                    if (scenario.name == "scan_resistance" && capacity == 100 &&
                        shards == 1) {
                        // 保留原确定性回归：该 trace 的理论最大命中率是 35.16%。
                        assert(tiny.hits > lru.hits);
                        assert(tiny.hit_rate() - lru.hit_rate() > 5.0);
                        checkedScanRegression = true;
                    }
                }
            }
        }
        if (options.traceFile.empty() &&
            std::find(options.capacities.begin(), options.capacities.end(), 100) !=
                options.capacities.end() &&
            std::find(options.shardCounts.begin(), options.shardCounts.end(), 1) !=
                options.shardCounts.end()) {
            assert(checkedScanRegression);
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "cache_policy_benchmark: " << ex.what() << '\n';
        return 1;
    }
}
