#pragma once

#include <cstddef>
#include <vector>

/**
 * @brief 固定容量连接池，使用空闲链表实现 O(1) 的获取和归还。
 *
 * 预分配 MAX_CONNECTIONS 个 T 类型对象，通过 acquire/release 管理生命周期。
 * 对象地址在整个池的生命周期内保持稳定，可以安全地使用裸指针引用。
 *
 * @tparam T 连接对象类型，需要支持默认构造
 */
template <typename T>
class ConnectionPool {
public:
    /// 最大连接数
    static constexpr size_t MAX_CONNECTIONS = 16384;

    /**
     * @brief 构造连接池，预分配全部连接对象并初始化空闲链表。
     */
    ConnectionPool()
        : connections_(MAX_CONNECTIONS),
          free_list_(MAX_CONNECTIONS)
    {
        for (size_t i = 0; i < MAX_CONNECTIONS; ++i)
            free_list_[i] = i;
    }

    /**
     * @brief 从池中获取一个可用连接。
     * @return 指向可用连接的指针，池满时返回 nullptr
     */
    T* acquire() noexcept {
        if (free_head_ >= MAX_CONNECTIONS)
            return nullptr;
        auto idx = free_list_[free_head_++];
        ++active_count_;
        return &connections_[idx];
    }

    /**
     * @brief 将连接归还到池中。
     * @param conn 指向使用 acquire 获取的连接对象的指针，必须是由本池分配的
     */
    void release(T* conn) noexcept {
        auto idx = static_cast<size_t>(conn - connections_.data());
        if (idx >= MAX_CONNECTIONS) return;
        free_list_[--free_head_] = idx;
        --active_count_;
    }

    /// @return 当前活跃（已借出）的连接数
    size_t active_count() const noexcept { return active_count_; }
    /// @return 池的容量（最大连接数）
    size_t capacity()  const noexcept { return MAX_CONNECTIONS; }

    // ---- 迭代器支持，用于遍历所有连接对象 ----

    T*       begin()       noexcept { return connections_.data(); }
    T*       end()         noexcept { return connections_.data() + MAX_CONNECTIONS; }
    const T* begin() const noexcept { return connections_.data(); }
    const T* end()   const noexcept { return connections_.data() + MAX_CONNECTIONS; }

private:
    /// 存储全部连接对象的连续数组，对象地址始终有效
    std::vector<T>        connections_;
    /// 空闲链表（索引栈），free_head_ 指向下一个可用的空闲索引
    std::vector<size_t>   free_list_;
    size_t                free_head_ = 0;
    size_t                active_count_ = 0;
};
