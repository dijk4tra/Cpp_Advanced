#include <iostream>
#include <iterator>
#include <sw/redis++/command_options.h>
#include <sw/redis++/redis++.h>
#include <sw/redis++/utils.h>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;
using namespace sw::redis;

int main()
{
    // 创建一个 Redis 对象, 连接 redis-server
    auto redis = Redis("tcp://127.0.0.1:6379");

    // ================ STRING 字符串命令 ================
    redis.set("key", "val");

    // 获取键对应的值, 返回 OptionalString 类型
    // OptionalString 可能包含值也可能为空
    auto val = redis.get("key");
    if (val) {
        // 如果包含值, 解引用获取 string 类型的值
        cout << *val << endl;
    } else {
        //否则键不存在
    }

    // ==================== LIST 列表命令 ====================
    // 使用 vector<string> 向列表中添加元素
    vector<string> vec = { "a", "b", "c"};
    redis.rpush("list", vec.begin(), vec.end());

    // 使用 initalizer_list 向列表中添加元素
    redis.rpush("list", { "a", "b", "c"});

    // 从 Redis LIST 读取数据到 vector<string>
    vec.clear();
    redis.lrange("list", 0, -1, back_inserter(vec));
    // LRANGE 0 -1 表示获取所有元素

    // ==================== HASH 哈希命令 ====================
    // 设置哈希表字段: hash 中的 field 字段值为 "val"
    redis.hset("hash", "field", "val");

    // 另一种设置方式: 使用 pair 键值对
    redis.hset("hash", make_pair("field", "val"));

    // 使用 unordered_map 批量设置哈希表
    unordered_map<string, string> m = {
        {"field1", "val1"},
        {"field2", "val2"}
    };
    redis.hset("hash", m.begin(), m.end()); // HSET 批量设置多个字段

    // 从哈希中读取数据到 unordered_map
    m.clear();
    redis.hgetall("hash", inserter(m, m.begin())); // HGETALL 获取所有字段和值

    // 只获取特定字段的值
    // 注意: 字段可能不存在, 所以需要使用 OptionalString 处理
    vector<OptionalString> vals;
    redis.hmget("hash", { "field1", "field2" }, back_inserter(vals)); // HMGET 获取多个字段

    // ==================== SET 集合命令 ====================
    // 向集合添加单个元素
    redis.sadd("set", "m1");

    // 使用 unordered_set 批量添加元素到集合
    unordered_set<string> set = { "m2", "m3" };
    redis.sadd("set", set.begin(), set.end());

    // 使用 initializer_list 批量添加元素
    redis.sadd("set", { "m2", "m3" });

    // 从集合中读取数据到 unorderer_set
    set.clear();
    redis.smembers("set", inserter(set, set.begin())); // SMEMBERS 获取集合所有成员

    // 检查元素是否存在集合中
    if (redis.sismember("set", "m1")) { // SISMEMBER 判断成员是否存在
        cout << "m1 exists" << endl;
    } // 否则不存在

    // ==================== SORTED SET 有序集合命令 ====================
    // 向有序集合添加单个元素（成员名和分数）
    redis.zadd("sorted_set", "m1", 1.3);

    // 使用 unordered_map 批量添加元素到有序集合
    unordered_map<string, double> scores = {
        { "m2", 2.3 },
        { "m3", 4.5 }
    };
    redis.zadd("sorted_set", scores.begin(), scores.end());

    // 从有序集合读取数据到 vector<pair<string, double>>
    // 注意：zrangebyscore 返回的结果是有序的，如果存储到 unordered_map 会丢失顺序
    vector<pair<string, double>> zset_result;
    redis.zrangebyscore("sorted_set",
        UnboundedInterval<double> {}, // 区间：(-inf, +inf)，即所有元素
        back_inserter(zset_result));

    // 只获取成员名 (不获取分数):
    // 传入 vector<string> 类型的插入迭代器作为输出参数
    vector<string> without_score;
    redis.zrangebyscore("sorted_set",
        BoundedInterval<double>(1.5, 3.4, BoundType::CLOSED), // 区间：[1.5, 3.4]，闭区间
        back_inserter(without_score));

    // 同时获取成员名和分数:
    // 传入 vector<pair<string, double>> 的 back_inserter
    vector<pair<string, double>> with_score;
    redis.zrangebyscore("sorted_set",
        BoundedInterval<double>(1.5, 3.4, BoundType::LEFT_OPEN), // 区间：(1.5, 3.4]，左开右闭
        back_inserter(with_score));

    // ==================== Transaction 事务 ====================
    // 创建一个事务对象
    auto tx = redis.transaction();

    // 在事务中运行多个命令, 并获得所有响应
    auto tx_replies = tx.incr("num0") // 对 num0 自增 1
                        .incr("num1") // 对 num1 自增 1
                        .mget({"num0", "num1"}) // 批量获取 num0 和 num1 的值
                        .exec(); // 执行事务

    // 解析事务响应, 通过响应类型和索引获取结果
    // 第 0 个命令 (incr) 的返回值是 long long 类型
    auto incr_result0 = tx_replies.get<long long>(0);

    // 第 1 个命令（incr）的返回值是 long long 类型
    auto incr_result1 = tx_replies.get<long long>(1);

    // 第 2 个命令（mget）的返回值是 vector<OptionalString> 类型
    vector<OptionalString> mget_cmd_result;
    tx_replies.get(2, back_inserter(mget_cmd_result));

    return 0;
}
