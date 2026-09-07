#include "redis_store.hpp"

#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "log.hpp"-

namespace {

    std::atomic<bool> g_ready{ false };
    std::unique_ptr<sw::redis::Redis> g_redis;

    // 统一把 redis++ 异常吞掉，按"不可用"降级处理（缓存层故障不能让服务挂掉）。
    // 说明：safe 失败返回 {}（即空 optional / false / 0），0 不会触发任何限流阈值。
    template <typename Fn>
    auto safe(Fn&& fn) -> decltype(fn()) {
        try {
            return fn();
        } catch (const std::exception& e) {
            // 避免刷屏：故障期间仅首条打日志
            static thread_local bool logged = false;
            if (!logged) {
                LOG_WARN(std::string("Redis operation failed (degraded): ") + e.what());
                logged = true;
            }
            return {};
        }
    }
}

namespace project {
namespace redis_store {

bool init(const std::string& uri, std::size_t pool_size) {
    try {
        // 用 URI 字符串构造（redis-plus-plus 内置解析 "tcp://host:port" / "unix://..." 等），
        // 避免依赖 ConnectionOptions::uri 等各版本不一致的成员，兼容更多库版本。
        // 连接池使用库默认大小（pool_size 参数保留以兼容旧调用）。
        (void)pool_size;
        g_redis = std::make_unique<sw::redis::Redis>(uri);
        // 探活
        (void)g_redis->ping();
        g_ready = true;
        LOG_INFO("Redis initialized: " + uri);
        return true;
    } catch (const std::exception& e) {
        g_ready = false;
        g_redis.reset();
        // 连接失败不致命：业务自动降级，仅告警提示（建议检查 redis-server 是否启动）
        LOG_WARN(std::string("Redis unavailable, service runs degraded (check redis-server): ") + e.what());
        return false;
    }
}

bool enabled() { return g_ready.load(std::memory_order_acquire) && g_redis != nullptr; }

std::optional<std::string> cache_get(const std::string& key) {
    if (!enabled()) return std::nullopt;
    return safe([&] { return g_redis->get(key); }); // 返回 optional<string>
}

bool cache_setex(const std::string& key, long ttl_sec, const std::string& value) {
    if (!enabled()) return false;
    // 注意：redis-plus-plus 的 setex 映射返回 void（SETEX 命令仅回 +OK），
    // 因此这里包装为返回 bool 的 lambda，safe() 才能用于 bool 返回值函数。
    return safe([&]() -> bool {
        g_redis->setex(key, std::chrono::seconds(ttl_sec), value);
        return true;
    });
}

bool cache_del(const std::string& key) {
    if (!enabled()) return false;
    return safe([&] { return g_redis->del(key) > 0; });
}

bool set_nx_ex(const std::string& key, const std::string& value, long ttl_sec) {
    if (!enabled()) return false;
    // SETNX + EXPIRE 组合（命令级 API，各版本一致）：
    // 仅在 key 不存在时写入；随后设置 TTL。两步非严格原子，对 10s 去重窗口可容忍。
    return safe([&] {
        if (!g_redis->setnx(key, value)) return false;
        (void)g_redis->expire(key, std::chrono::seconds(ttl_sec));
        return true;
    });
}

bool key_exists(const std::string& key) {
    if (!enabled()) return false;
    return safe([&] { return g_redis->exists(key) > 0; });
}

long long counter_incr(const std::string& key) {
    if (!enabled()) return 0;
    return safe([&] { return g_redis->incr(key); });
}

long long counter_incrby(const std::string& key, long long delta) {
    if (!enabled()) return 0;
    return safe([&] { return g_redis->incrby(key, delta); });
}

bool key_expire(const std::string& key, long ttl_sec) {
    if (!enabled()) return false;
    return safe([&] { return g_redis->expire(key, std::chrono::seconds(ttl_sec)); });
}

long long get_long(const std::string& key) {
    if (!enabled()) return 0;
    return safe([&] {
        auto v = g_redis->get(key);
        if (!v) return 0LL;
        try { return std::stoll(*v); } catch (...) { return 0LL; }
    });
}

long long incr_with_expire(const std::string& key, long ttl_sec) {
    if (!enabled()) return 0;
    // 计数 +1；若 key 是新建的（第一次失败），顺带设置 TTL
    return safe([&] {
        long long c = g_redis->incr(key);
        if (c == 1) {
            (void)g_redis->expire(key, std::chrono::seconds(ttl_sec));
        }
        return c;
    });
}

bool set_add(const std::string& key, const std::string& member) {
    if (!enabled()) return false;
    return safe([&] { return g_redis->sadd(key, member) > 0; });
}

bool set_remove(const std::string& key, const std::string& member) {
    if (!enabled()) return false;
    return safe([&] { return g_redis->srem(key, member) > 0; });
}

bool set_contains(const std::string& key, const std::string& member) {
    if (!enabled()) return false;
    return safe([&] { return g_redis->sismember(key, member) > 0; });
}

} // namespace redis_store
} // namespace project
