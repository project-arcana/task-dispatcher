#pragma once

#include <algorithm>
#include <atomic>
#include <memory>

namespace td::container
{
// Chase-Lev workstealing queue/deque
// MIT Licensed, from:
// https://github.com/ssbl/concurrent-deque
namespace spmc
{
template <typename T>
class Buffer
{
private:
    long id_;
    int log_size;
    T* segment;
    Buffer<T>* next;

public:
    Buffer(int ls, long id) : id_(id), log_size(ls), segment(new T[1 << log_size]), next(nullptr) {}

    ~Buffer() { delete[] segment; }

    [[nodiscard]] long id() const { return id_; }

    [[nodiscard]] Buffer<T>* next_buffer() { return next; }

    [[nodiscard]] size_t size() const { return static_cast<size_t>(1 << log_size); }

    [[nodiscard]] T get(size_t i) const { return segment[i % size()]; }

    void put(size_t i, T const& item) { segment[i % size()] = item; }

    [[nodiscard]] Buffer<T>* resize(long b, long t, int delta)
    {
        auto buffer = new Buffer<T>(log_size + delta, id_ + 1);
        for (auto i = t; i < b; ++i)
            buffer->put(i, get(i));
        next = buffer;
        return buffer;
    }
};

// A buffer_tls is created for each stealer thread. It is intended to
// be local to that thread; the reclaimer creates one of these
// whenever a stealer thread is created.
struct buffer_tls
{
    // The id of the buffer last used by the thread.
    std::atomic<long> id_last_used;
    // If set, we don't check `id_last_used`.
    std::atomic<bool> was_idle;
    // The next buffer_tls in the list.
    buffer_tls* next;
};

// The reclaimer deals with additions to and cleanup of the
// `buffer_tls` list.
class Reclaimer
{
private:
    std::atomic<buffer_tls*> id_list;

public:
    Reclaimer() : id_list(nullptr) {}

    ~Reclaimer()
    {
        auto head = id_list.load(std::memory_order_relaxed);
        while (head)
        {
            auto reclaimed = head;
            head = head->next;
            delete reclaimed;
        }
    }

    [[nodiscard]] buffer_tls* get_id_list() { return id_list.load(std::memory_order_relaxed); }

    // Each stealer thread registers before using the deque.
    [[nodiscard]] buffer_tls* register_thread()
    {
        auto tls = new buffer_tls{{0}, {true}, nullptr};
        tls->next = get_id_list();

        while (!id_list.compare_exchange_weak(tls->next, tls))
        {
            tls->next = id_list;
        }

        return tls;
    }
};

template <typename T>
class Deque
{
private:
    std::atomic<long> top;
    std::atomic<long> bottom;
    Buffer<T>* unlinked;
    int const log_initial_size;

public:
    Reclaimer reclaimer;
    std::atomic<Buffer<T>*> buffer;

    Deque(int log_reserve_size = 4)
      : top(0), bottom(0), unlinked(), log_initial_size(log_reserve_size), reclaimer(), buffer(new Buffer<T>(log_initial_size, 0))
    {
    }

    ~Deque()
    {
        auto b = buffer.load(std::memory_order_relaxed);

        while (unlinked && unlinked != b)
        {
            auto reclaimed = unlinked;
            unlinked = unlinked->next_buffer();
            delete reclaimed;
        }

        delete b;
    }

    void push_bottom(const T& object)
    {
        auto b = bottom.load(std::memory_order_relaxed);
        auto t = top.load(std::memory_order_acquire);
        auto a = buffer.load(std::memory_order_relaxed);

        auto size = b - t;
        if (size >= a->size() - 1)
        {
            unlinked = unlinked ? unlinked : a;
            a = a->resize(b, t, 1);
            buffer.store(a, std::memory_order_release);
        }

        if (unlinked)
            reclaim_buffers(a);

        a->put(b, object);
        // This fence ensures that an object isn't stolen before we update
        // `bottom`.
        std::atomic_thread_fence(std::memory_order_release);
        bottom.store(b + 1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool pop_bottom(T& out_ref)
    {
        auto b = bottom.load(std::memory_order_relaxed);
        auto a = buffer.load(std::memory_order_acquire);

        bottom.store(b - 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto t = top.load(std::memory_order_relaxed);

        auto size = b - t;
        auto success = false;

        if (size <= 0)
        {
            // Deque empty: reverse the decrement to bottom.
            bottom.store(b, std::memory_order_relaxed);
        }
        else if (size == 1)
        {
            // Race against steals.
            if (top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                out_ref = a->get(t);
                success = true;
            }
            bottom.store(b, std::memory_order_relaxed);
        }
        else
        {
            out_ref = a->get(b - 1);
            success = true;

            if (size <= a->size() / 3 && size > 1 << log_initial_size)
            {
                unlinked = unlinked ? unlinked : a;
                a = a->resize(b, t, -1);
                buffer.store(a, std::memory_order_release);
            }

            if (unlinked)
                reclaim_buffers(a);
        }

        return success;
    }

    [[nodiscard]] bool steal(T& out_ref)
    {
        auto t = top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto b = bottom.load(std::memory_order_acquire);

        int size = b - t;

        if (size > 0)
        {
            auto a = buffer.load(std::memory_order_consume);
            // Race against other steals and a pop.
            if (top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                out_ref = a->get(t);
                return true;
            }
        }

        return false;
    }

    // An experimental mechanism to reclaim unlinked buffers. Each
    // stealer thread keeps track of the id of the buffer it last read
    // from. We reclaim all buffers with id strictly less than the
    // minimum of the ids seen by the stealers.
    //
    // `new_buffer` points to the current buffer.
    //
    // XXX: Ideally we shouldn't need the pointer to the new buffer.
    void reclaim_buffers(Buffer<T>* new_buffer)
    {
        auto min_id = new_buffer->id();
        auto head = reclaimer.get_id_list();

        while (head)
        {
            auto idle = head->was_idle.load(std::memory_order_acquire);
            if (!idle)
            {
                auto last_used_id = head->id_last_used.load(std::memory_order_relaxed);
                min_id = std::min(min_id, last_used_id);
            }
            head = head->next;
        }

        while (unlinked->id() < min_id)
        {
            auto reclaimed = unlinked;
            unlinked = unlinked->next_buffer();
            delete reclaimed;
        }
    }
};

template <typename T>
class Worker
{
private:
    std::shared_ptr<Deque<T>> deque;

public:
    explicit Worker() : deque(nullptr) {} // invalid state default ctor
    // Deferred init setter
    void setDeque(std::shared_ptr<Deque<T>> d) { deque = std::move(d); }

    // Regular ctor
    explicit Worker(std::shared_ptr<Deque<T>> d) : deque(std::move(d)) {}


    // Copy constructor.
    // There can only be one worker end.
    Worker(const Worker<T>& w) = delete;

    // Move constructor.
    Worker(Worker<T>&& w) : deque(std::move(w.deque)) {}

    ~Worker() = default;

    void push(const T& item) { deque->push_bottom(item); }

    [[nodiscard]] bool pop(T& out_ref) { return deque->pop_bottom(out_ref); }
};

template <typename T>
class Stealer
{
private:
    std::shared_ptr<Deque<T>> deque;
    buffer_tls* buffer_data;

public:
    explicit Stealer() : deque(nullptr), buffer_data(nullptr) {} // invalid state default ctor
    // Deferred init setter
    void setDeque(std::shared_ptr<Deque<T>> d)
    {
        deque = std::move(d);
        buffer_data = deque->reclaimer.register_thread();
    }

    // Regular ctor
    explicit Stealer(std::shared_ptr<Deque<T>> d) : deque(std::move(d)), buffer_data(deque->reclaimer.register_thread()) {}

    // Copy constructor.
    //
    // Used whenever a new stealer thread is created.
    Stealer(const Stealer<T>& s) : deque(s.deque), buffer_data(deque->reclaimer.register_thread()) {}

    // Move constructor.
    //
    // Used when we're passing the stealer end around in the same
    // thread.
    Stealer(Stealer<T>&& s) : deque(std::move(s.deque)), buffer_data(s.buffer_data) {}

    ~Stealer() = default;

    [[nodiscard]] bool steal(T& out_ref)
    {
        // We use memory_order_release to synchronize with the read by the
        // reclaimer. It makes sense, but I'm not absolutely sure about
        // this.
        buffer_data->was_idle.store(false, std::memory_order_release);
        auto stolen = deque->steal(out_ref);
        buffer_data->was_idle.store(true, std::memory_order_release);

        // Stealers load the buffer pointer using memory_order_consume.
        auto b = deque->buffer.load(std::memory_order_consume);
        buffer_data->id_last_used.store(b->id(), std::memory_order_relaxed);

        return stolen;
    }
};
}

// Create a worker and stealer end for a single deque. The stealer end
// can be cloned when spawning stealer threads.
//
// auto ws = deque::deque<T>();
// auto worker = std::move(ws.first);
// auto stealer = std::move(ws.second);
//
// std::thread foo([&stealer]() {
//   auto clone = stealer;
//   auto work = clone.steal();
//   /* ... */
// });
//
// foo.join();
//
template <typename T>
std::pair<spmc::Worker<T>, spmc::Stealer<T>> make_spmc_queue(int reserve_size = 4)
{
    auto d = std::make_shared<spmc::Deque<T>>(reserve_size);
    return {spmc::Worker<T>(d), spmc::Stealer<T>(d)};
}

} // namespace deque
