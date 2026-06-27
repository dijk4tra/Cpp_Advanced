#include "../../include/cache/TinyLfuFrequencySketch.h"

#include <algorithm>
#include <limits>

namespace
{
// 每一行使用不同种子扰动同一个 key 哈希值，降低多行发生相同碰撞的概率。
constexpr std::uint64_t kRowSeeds[] = {
    0x9e3779b97f4a7c15ULL,
    0xc2b2ae3d27d4eb4fULL,
    0x165667b19e3779f9ULL,
    0x85ebca77c2b2ae63ULL
};

constexpr std::uint64_t kDoorkeeperSeed = 0xd6e8feb86659fd93ULL;
}

/**
 * @brief 按预期缓存容量初始化 Count-Min Sketch 和 Doorkeeper。
 *
 * 初始化列表严格按照成员在头文件中的声明顺序构造。后面的 widthMask_ 和
 * doorkeeperBitMask_ 可以直接使用已经构造完成的 width_、doorkeeper_，避免在
 * 构造函数体内先创建空 vector 再重新分配。
 */
TinyLfuFrequencySketch::TinyLfuFrequencySketch(std::size_t expectedCapacity,
                                               std::size_t sampleSize)
    // 每行宽度约为容量的 2 倍，并向上取 2 的幂。较低负载因子可减少碰撞导致的
    // 频率高估；最小宽度 16 让极小缓存也有基本区分能力。
    : width_(next_power_of_two(std::max<std::size_t>(16, expectedCapacity * 2))),
      widthMask_(width_ - 1),
      // 总计 kRows * width_ 个 4-bit 计数器。加 1 再除 2 可兼容奇数个计数器，
      // vector 的第二个参数 0 表示把每个压缩字节初始化为 0。
      counters_((kRows * width_ + 1) / 2, 0),
      // Doorkeeper 使用约每个缓存条目 8 bit，并至少分配一个 uint64_t。
      doorkeeper_(next_power_of_two(std::max<std::size_t>(64,
                                                          expectedCapacity * 8)) /
                      64,
                  0),
      doorkeeperBitMask_(doorkeeper_.size() * 64 - 1),
      sampleSize_(std::max<std::size_t>(1, sampleSize))
{}

/**
 * @brief 记录一次访问，并在采样周期结束时执行频率老化。
 */
bool TinyLfuFrequencySketch::increment(std::uint64_t hash)
{
    // Doorkeeper 不保存计数，只判断“本周期是否见过”。第一次访问仅设置 Bloom
    // 位，不污染 Count-Min Sketch；从第二次访问开始才增加 4 行计数器。
    if (!doorkeeper_contains(hash)) {
        doorkeeper_put(hash);
    } else {
        // 同一个 key 在 4 行各增加一个计数器。只有所有行都发生碰撞，最终取最小值
        // 时才会持续高估该 key 的频率。
        for (std::size_t row = 0; row < kRows; ++row) {
            increment_counter(counter_index(hash, row));
        }
    }

    ++sampleCount_;
    if (sampleCount_ < sampleSize_) {
        return false;
    }

    reset();
    return true;
}

/**
 * @brief 估算指定哈希值的访问频率。
 *
 * Count-Min Sketch 发生碰撞时只可能高估，因此取 4 行最小值可以选择受碰撞影响
 * 最小的一行。该函数只读取预分配数组，不修改状态。
 */
std::uint32_t TinyLfuFrequencySketch::estimate(std::uint64_t hash) const
{
    std::uint8_t minimum = kCounterMax;
    for (std::size_t row = 0; row < kRows; ++row) {
        minimum = std::min(minimum, counter(counter_index(hash, row)));
    }

    // Doorkeeper 中存在表示至少访问过一次。Bloom Filter 可能产生假阳性，因此
    // TinyLFU 只把它作为 +1 的弱证据，不会无限放大误差。
    return static_cast<std::uint32_t>(minimum) + (doorkeeper_contains(hash) ? 1U : 0U);
}

/**
 * @brief 计算两个动态数组当前预留存储区的字节数。
 */
std::size_t TinyLfuFrequencySketch::memory_usage_bytes() const
{
    // 使用 capacity() 而不是 size()，因为 vector 实际占用的是预留容量；当前实现
    // 构造后不再扩容，所以二者通常相等。
    return counters_.capacity() * sizeof(std::uint8_t) +
           doorkeeper_.capacity() * sizeof(std::uint64_t);
}

/**
 * @brief 把正整数向上取整到最接近的 2 的幂。
 */
std::size_t TinyLfuFrequencySketch::next_power_of_two(std::size_t value)
{
    if (value <= 1) {
        return 1;
    }

    // 逐次把最高位以下的 bit 全部置 1，最后加 1 得到向上取整的 2 的幂。
    --value;
    // shift 依次为 1、2、4、8...。每轮把已知最高位向右扩散一倍范围。
    // shift <<= 1 等价于 shift = shift * 2。
    for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
        value |= value >> shift;
    }
    // 若扩散后所有 bit 都是 1，再加 1 会溢出。此时返回 size_t 可表示的最大 2 次幂。
    if (value == std::numeric_limits<std::size_t>::max()) {
        return std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
    }
    return value + 1;
}

/**
 * @brief 对已有 64 位哈希执行雪崩混合。
 *
 * 异或右移和奇数常量乘法会让输入任意一位变化扩散到多个输出位。ULL 后缀保证
 * 常量按 unsigned long long 参与运算，不会被有符号溢出规则影响。
 */
std::uint64_t TinyLfuFrequencySketch::mix(std::uint64_t value)
{
    // SplitMix64 的 finalizer 可把 std::hash 输出中的相邻模式进一步打散。
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

/**
 * @brief 将二维的 row/column 坐标转换为 counters_ 的一维计数器下标。
 */
std::size_t TinyLfuFrequencySketch::counter_index(std::uint64_t hash,
                                                  std::size_t row) const
{
    // 每一行加入不同 seed 后再混合。widthMask_ 等于 width_-1，width_ 又是 2 的幂，
    // 因此按位与的结果一定处于 [0, width_)。
    const std::size_t column = static_cast<std::size_t>(mix(hash + kRowSeeds[row])) &
                               widthMask_;
    return row * width_ + column;
}

/**
 * @brief 从压缩数组中读取一个 4-bit 计数器。
 */
std::uint8_t TinyLfuFrequencySketch::counter(std::size_t index) const
{
    // index/2 定位所属字节；index&1 判断位于低半字节还是高半字节。
    const std::uint8_t packed = counters_[index / 2];
    return (index & 1U) == 0 ? static_cast<std::uint8_t>(packed & 0x0fU)
                             : static_cast<std::uint8_t>((packed >> 4) & 0x0fU);
}

/**
 * @brief 对指定 4-bit 计数器执行饱和加一。
 */
void TinyLfuFrequencySketch::increment_counter(std::size_t index)
{
    // 引用直接绑定 vector 中的压缩字节，后面对 packed 的修改会写回 counters_。
    std::uint8_t& packed = counters_[index / 2];
    const bool high = (index & 1U) != 0;
    const std::uint8_t value = high ? static_cast<std::uint8_t>((packed >> 4) & 0x0fU)
                                    : static_cast<std::uint8_t>(packed & 0x0fU);
    if (value == kCounterMax) {
        return;
    }

    // 对应半字节未饱和时加 1。高半字节的步长是 0x10，低半字节是 0x01。
    packed = static_cast<std::uint8_t>(packed + (high ? 0x10U : 0x01U));
}

/**
 * @brief 查询 Doorkeeper 中 key 对应的两个 Bloom bit。
 */
bool TinyLfuFrequencySketch::doorkeeper_contains(std::uint64_t hash) const
{
    const std::size_t bit1 = static_cast<std::size_t>(mix(hash)) & doorkeeperBitMask_;
    const std::size_t bit2 = static_cast<std::size_t>(mix(hash + kDoorkeeperSeed)) &
                             doorkeeperBitMask_;
    // bit/64 定位 uint64_t 元素，bit%64 定位元素内部偏移；1 左移后得到单 bit 掩码。
    const bool first = (doorkeeper_[bit1 / 64] & (std::uint64_t{1} << (bit1 % 64))) != 0;
    const bool second = (doorkeeper_[bit2 / 64] & (std::uint64_t{1} << (bit2 % 64))) != 0;
    return first && second;
}

/**
 * @brief 在 Doorkeeper 中设置 key 对应的两个 Bloom bit。
 */
void TinyLfuFrequencySketch::doorkeeper_put(std::uint64_t hash)
{
    const std::size_t bit1 = static_cast<std::size_t>(mix(hash)) & doorkeeperBitMask_;
    const std::size_t bit2 = static_cast<std::size_t>(mix(hash + kDoorkeeperSeed)) &
                             doorkeeperBitMask_;
    // |= 只会把目标 bit 置 1，不会清除同一个 uint64_t 中其他 key 已设置的 bit。
    doorkeeper_[bit1 / 64] |= std::uint64_t{1} << (bit1 % 64);
    doorkeeper_[bit2 / 64] |= std::uint64_t{1} << (bit2 % 64);
}

/**
 * @brief 执行一次 Frequency Aging。
 */
void TinyLfuFrequencySketch::reset()
{
    // 每个字节包含两个 4-bit counter。右移一位并用 0x77 屏蔽，可同时完成两个
    // 半字节的无符号除 2，且不会让高半字节最低位串入低半字节。
    for (std::uint8_t& packed : counters_) {
        packed = static_cast<std::uint8_t>((packed >> 1) & 0x77U);
    }
    // 新采样周期重新判断“首次访问”，因此 Bloom Filter 必须整体清零。
    std::fill(doorkeeper_.begin(), doorkeeper_.end(), 0);
    sampleCount_ = 0;
    ++resetCount_;
}
