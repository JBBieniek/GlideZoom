#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace glide::input
{

    // Single producer and single consumer; one slot stays empty to distinguish
    // full from empty. The producer never overwrites an unread event.
#if defined(_MSC_VER)
#pragma warning(push)
    // Separate cache lines for the producer and consumer are intentional.
#pragma warning(disable : 4324)
#endif
    template<class T, std::size_t Size>
    class SpscQueue
    {
        static_assert(Size >= 2 && std::is_trivially_copyable_v<T>);

    public:
        bool Push(const T& value) noexcept
        {
            const auto head = head_.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Size;
            if (next == tail_.load(std::memory_order_acquire))
                return false;
            values_[head] = value;
            head_.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(T& value) noexcept
        {
            const auto tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire))
                return false;
            value = values_[tail];
            tail_.store((tail + 1) % Size, std::memory_order_release);
            return true;
        }
        // Only when both producer and consumer are stopped.
        void Reset() noexcept
        {
            head_.store(0, std::memory_order_relaxed);
            tail_.store(0, std::memory_order_relaxed);
        }

    private:
        std::array<T, Size> values_{};
        alignas(64) std::atomic<std::size_t> head_{ 0 };
        alignas(64) std::atomic<std::size_t> tail_{ 0 };
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    struct Snapshot
    {
        std::uint64_t options = 0;
        std::uint64_t epoch = 0;
        double zoom = 1;
    };

    // Single writer; bounded reader, with no retry or lock. All fields are atomic
    // to avoid C++ data races. Sequential consistency provides one total order for
    // the odd/even revision and its payload, including on weaker memory models.
    class SnapshotChannel
    {
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::atomic<double>::is_always_lock_free);

    public:
        void Publish(Snapshot value) noexcept
        {
            revision_.fetch_add(1);
            options_.store(value.options);
            epoch_.store(value.epoch);
            zoom_.store(value.zoom);
            revision_.fetch_add(1);
        }
        bool Read(Snapshot& value) const noexcept
        {
            const auto before = revision_.load();
            if (before & 1)
                return false;
            const Snapshot candidate{ options_.load(), epoch_.load(), zoom_.load() };
            if (revision_.load() != before)
                return false;
            value = candidate;
            return true;
        }

    private:
        std::atomic<std::uint64_t> revision_{ 0 };
        std::atomic<std::uint64_t> options_{ (400ull << 16) | (30000ull << 32) }, epoch_{ 0 };
        std::atomic<double> zoom_{ 1 };
    };

} // namespace glide::input
