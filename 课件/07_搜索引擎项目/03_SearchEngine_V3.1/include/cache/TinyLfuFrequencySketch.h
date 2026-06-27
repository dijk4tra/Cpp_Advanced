#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief W-TinyLFU 使用的固定空间访问频率估计器。
 *
 * 该类组合三个组件：
 *
 * 1. Count-Min Sketch：4 行计数器分别使用不同哈希扰动，查询时取最小值；
 * 2. Doorkeeper：Bloom Filter 记录采样周期内第一次访问，避免一次性 key 写入 Sketch；
 * 3. Frequency Aging：达到采样阈值后将全部计数减半并清空 Doorkeeper。
 *
 * Count-Min Sketch 的每个计数器只占 4 bit，最大值为 15。估算值再加上
 * Doorkeeper 中的首次访问位，因此 estimate() 的范围是 [0, 16]。所有内存均在
 * 构造时按 expectedCapacity 分配，后续访问任意数量的不同 key 都不会扩容。
 *
 * 本类本身不加锁。ShardedWTinyLfuCache 在调用它时已经持有对应分片锁，避免
 * 每次访问重复加锁。
 */
class TinyLfuFrequencySketch
{
public:
    /**
     * @param expectedCapacity 当前分片预计容纳的缓存条目数。
     * @param sampleSize 累计多少次访问后执行一次频率老化。
     */
    TinyLfuFrequencySketch(std::size_t expectedCapacity, std::size_t sampleSize);

    /**
     * @brief 记录一次访问。
     * @param hash 已由调用方计算的 key 哈希值。
     * @return 本次记录是否触发了 Frequency Aging。
     */
    bool increment(std::uint64_t hash);

    /**
     * @brief 返回 key 的近似访问频率，范围为 [0, 16]。
     */
    std::uint32_t estimate(std::uint64_t hash) const;

    /** @brief 返回触发一次老化所需的访问样本数。 */
    std::size_t sample_size() const { return sampleSize_; }

    /** @brief 返回从构造到当前一共执行过多少次 Frequency Aging。 */
    std::size_t reset_count() const { return resetCount_; }

    /**
     * @brief 返回 Sketch 和 Doorkeeper 预分配存储区占用的字节数。
     *
     * 这里只统计两个 vector 的元素存储区，不包含 vector 对象自身的少量元数据。
     */
    std::size_t memory_usage_bytes() const;

private:
    // Count-Min Sketch 固定使用 4 行；行数越多，碰撞同时发生在所有行的概率越低。
    static constexpr std::size_t kRows = 4;

    // 4 bit 能表示 0~15，因此计数器达到 15 后采用饱和加法，不再继续增长。
    static constexpr std::uint8_t kCounterMax = 15;

    /** @brief 将 value 向上取整为 2 的幂，便于后续使用位与代替取模。 */
    static std::size_t next_power_of_two(std::size_t value);

    /** @brief 使用 SplitMix64 finalizer 进一步打散调用方传入的哈希值。 */
    static std::uint64_t mix(std::uint64_t value);

    /** @brief 计算指定行中 key 对应计数器的一维数组下标。 */
    std::size_t counter_index(std::uint64_t hash, std::size_t row) const;

    /** @brief 从压缩字节中读取指定的 4-bit 计数器。 */
    std::uint8_t counter(std::size_t index) const;

    /** @brief 将指定 4-bit 计数器加一；达到 15 后保持饱和。 */
    void increment_counter(std::size_t index);

    /** @brief 判断 Doorkeeper 的两个 Bloom bit 是否都已设置。 */
    bool doorkeeper_contains(std::uint64_t hash) const;

    /** @brief 设置 key 对应的两个 Bloom bit，记录本采样周期内的首次访问。 */
    void doorkeeper_put(std::uint64_t hash);

    /** @brief 将全部计数器减半、清空 Doorkeeper，并开始新的采样周期。 */
    void reset();

private:
    // Count-Min Sketch 每一行的计数器数量。它始终是 2 的幂。
    std::size_t width_;

    // width_ - 1；column = hash & widthMask_ 等价于 hash % width_，但开销更低。
    std::size_t widthMask_;

    // 两个 4-bit counter 打包进一个字节：偶数下标在低 4 位，奇数在高 4 位。
    std::vector<std::uint8_t> counters_;

    // Bloom Filter 位数组。uint64_t 的每一位都可表示一个 Doorkeeper 标记。
    std::vector<std::uint64_t> doorkeeper_;

    // Doorkeeper 总 bit 数减 1；总 bit 数是 2 的幂，因此也可用位与代替取模。
    std::size_t doorkeeperBitMask_;

    // 一个采样周期允许记录的访问次数，至少为 1。
    std::size_t sampleSize_;

    // 当前采样周期已经记录的访问数；达到 sampleSize_ 时触发 reset()。
    std::size_t sampleCount_ = 0;

    // 已执行的老化次数，主要供测试和运行统计使用。
    std::size_t resetCount_ = 0;
};
