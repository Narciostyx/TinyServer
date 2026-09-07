#include <optional>
#include <string>

namespace project {
namespace redis_store {

    // 初始化（uri 形如 "tcp://127.0.0.1:6379"）。返回 false 表示不可用（非致命，走降级）。
    bool init(const std::string& uri, std::size_t pool_size = 8);

    // Redis 是否已启用且可用
    bool enabled();

    // ---------- 缓存（cache-aside，值一律存字符串 / JSON）----------

    // 命中返回缓存值；未命中或不可用返回 std::nullopt（调用方回源 DB）
    std::optional<std::string> cache_get(const std::string& key);

    // 写缓存并设置 TTL（秒）。返回是否成功
    bool cache_setex(const std::string& key, long ttl_sec, const std::string& value);

    // 删除缓存键（写路径失效缓存用）。返回是否存在被删除
    bool cache_del(const std::string& key);

    // SET key val NX EX ttl：仅当 key 不存在时写入。
    // 返回 true 表示本次写入成功（可视为"首次"）。用于浏览量去重/限流窗口。
    bool set_nx_ex(const std::string& key, const std::string& value, long ttl_sec);

    // key 是否存在（限流/黑名单判断）
    bool key_exists(const std::string& key);

    // ---------- 计数 ----------

    // INCR；不可用或出错返回 0（小于任何业务阈值，不会误触发限流）
    long long counter_incr(const std::string& key);
    // INCRBY
    long long counter_incrby(const std::string& key, long long delta);
    // 设置 TTL（秒）
    bool key_expire(const std::string& key, long ttl_sec);
    // GET 并解析为整数；不可用/不存在返回 0
    long long get_long(const std::string& key);
    // 原子"计数 +1，若此前不存在则同时设置 TTL"（失败计数限流的惯用写法）
    long long incr_with_expire(const std::string& key, long ttl_sec);

    // ---------- Set（点赞关系、成员集合）----------

    bool set_add(const std::string& key, const std::string& member);
    bool set_remove(const std::string& key, const std::string& member);
    bool set_contains(const std::string& key, const std::string& member);

} // namespace redis_store
} // namespace project
