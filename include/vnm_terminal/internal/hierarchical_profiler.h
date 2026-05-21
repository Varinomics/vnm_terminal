#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

inline constexpr std::string_view k_profile_root_scope_name    = "[root]";
inline constexpr std::string_view k_profile_unnamed_scope_name = "[unnamed]";

struct Profile_node_snapshot
{
    std::string                        name;
    std::uint64_t                      call_count = 0U;
    std::chrono::nanoseconds           total_time = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds           self_time  = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds           child_time = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds           min_time   = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds           max_time   = std::chrono::nanoseconds::zero();
    std::vector<Profile_node_snapshot> children;
};

struct Profile_timeline_scope_snapshot
{
    std::string                name;
    std::uint64_t              call_count = 0U;
    std::chrono::nanoseconds   total_time = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds   min_time   = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds   max_time   = std::chrono::nanoseconds::zero();
};

struct Profile_timeline_bucket_snapshot
{
    std::chrono::milliseconds                    start_time;
    std::chrono::milliseconds                    end_time;
    std::vector<Profile_timeline_scope_snapshot> scopes;
};

struct Profile_timeline_snapshot
{
    std::chrono::milliseconds  bucket_width;
    std::vector<Profile_timeline_bucket_snapshot>
                               buckets;
};

// Active-profiler binding is thread-local. Recording is owned by one thread tree;
// cross-thread snapshots are serialized for render-thread handoff.
class Hierarchical_profiler
{
public:
    using clock_type     = std::chrono::steady_clock;
    using time_point     = clock_type::time_point;
    using clock_function = time_point (*)();

    struct scope_token_t
    {
        std::uint64_t generation  = 0U;
        std::uint64_t id          = 0U;
    };

    Hierarchical_profiler()
    :
        Hierarchical_profiler(default_clock)
    {}

    explicit Hierarchical_profiler(clock_function clock)
    :
        m_clock(clock == nullptr ? default_clock : clock)
    {
        m_root.name = std::string(k_profile_root_scope_name);
    }

    Hierarchical_profiler(const Hierarchical_profiler&)            = delete;
    Hierarchical_profiler& operator=(const Hierarchical_profiler&) = delete;
    Hierarchical_profiler(Hierarchical_profiler&&)                 = delete;
    Hierarchical_profiler& operator=(Hierarchical_profiler&&)      = delete;

    // Clears completed data and drops in-progress scopes; later unmatched ends are no-ops.
    void reset()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_root.call_count = 0U;
        m_root.total_time = std::chrono::nanoseconds::zero();
        m_root.min_time   = std::chrono::nanoseconds::zero();
        m_root.max_time   = std::chrono::nanoseconds::zero();
        m_active_scopes.clear();
        m_root.children.clear();
        m_timeline_buckets.clear();
        m_timeline_origin = m_clock();
        ++m_generation;
    }

    scope_token_t begin_scope(std::string_view name)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        Node& parent = m_active_scopes.empty()
            ? m_root
            : *m_active_scopes.back().node;
        Node& node = child_for_name(parent, name);
        const scope_token_t token{m_generation, ++m_next_scope_id};
        m_active_scopes.push_back({&node, m_clock(), token});
        return token;
    }

    void end_scope()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active_scopes.empty()) {
            return;
        }

        end_active_scope(m_active_scopes.back());
    }

    void end_scope(scope_token_t token)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active_scopes.empty()) {
            return;
        }

        const Active_scope& active_scope = m_active_scopes.back();
        if (active_scope.token.generation != token.generation ||
            active_scope.token.id         != token.id)
        {
            return;
        }

        end_active_scope(active_scope);
    }

    Profile_node_snapshot root_snapshot() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // The synthetic root is not a measured scope; it totals its children and has no self time.
        const time_point snapshot_time = m_clock();
        Profile_node_snapshot root = snapshot_node(m_root, snapshot_time);
        root.total_time                = root.child_time;
        root.self_time                 = std::chrono::nanoseconds::zero();
        return root;
    }

    std::vector<Profile_node_snapshot> snapshot() const
    {
        return root_snapshot().children;
    }

    Profile_timeline_snapshot timeline_snapshot() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);

        Profile_timeline_snapshot out;
        out.bucket_width = m_timeline_bucket_width;
        for (const Timeline_bucket& bucket : m_timeline_buckets) {
            if (bucket.scopes.empty()) {
                continue;
            }

            Profile_timeline_bucket_snapshot bucket_snapshot;
            const std::chrono::milliseconds bucket_offset =
                m_timeline_bucket_width * bucket.index;
            bucket_snapshot.start_time = bucket_offset;
            bucket_snapshot.end_time   = bucket_snapshot.start_time + m_timeline_bucket_width;
            bucket_snapshot.scopes.reserve(bucket.scopes.size());
            for (const Timeline_scope& scope : bucket.scopes) {
                bucket_snapshot.scopes.push_back({
                    scope.name,
                    scope.call_count,
                    scope.total_time,
                    scope.min_time,
                    scope.max_time,
                });
            }
            out.buckets.push_back(std::move(bucket_snapshot));
        }

        return out;
    }

    static Hierarchical_profiler* active_profiler()
    {
        return active_profiler_slot();
    }

    static Hierarchical_profiler* bind_active_profiler(Hierarchical_profiler* profiler)
    {
        Hierarchical_profiler*& slot = active_profiler_slot();
        Hierarchical_profiler* previous = slot;
        slot = profiler;
        return previous;
    }

private:
    struct Node
    {
        std::string                    name;
        std::uint64_t                  call_count      = 0U;
        std::chrono::nanoseconds       total_time      = std::chrono::nanoseconds::zero();
        std::chrono::nanoseconds       min_time        = std::chrono::nanoseconds::zero();
        std::chrono::nanoseconds       max_time        = std::chrono::nanoseconds::zero();
        std::vector<std::unique_ptr<Node>>
                                       children;
    };

    struct Active_scope
    {
        Node*                          node            = nullptr;
        time_point                     start_time;
        scope_token_t                  token;
    };

    struct Timeline_scope
    {
        std::string                    name;
        std::uint64_t                  call_count = 0U;
        std::chrono::nanoseconds       total_time = std::chrono::nanoseconds::zero();
        std::chrono::nanoseconds       min_time   = std::chrono::nanoseconds::zero();
        std::chrono::nanoseconds       max_time   = std::chrono::nanoseconds::zero();
    };

    struct Timeline_bucket
    {
        std::uint64_t                  index      = 0U;
        std::vector<Timeline_scope>    scopes;
    };

    void end_active_scope(const Active_scope& active_scope)
    {
        const time_point end_time = m_clock();
        const std::chrono::nanoseconds elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time -
            active_scope.start_time);
        if (active_scope.node->call_count == 0U || elapsed < active_scope.node->min_time) {
            active_scope.node->min_time = elapsed;
        }
        if (active_scope.node->call_count == 0U || elapsed > active_scope.node->max_time) {
            active_scope.node->max_time = elapsed;
        }
        ++active_scope.node->call_count;
        active_scope.node->total_time += elapsed;
        record_timeline_scope(active_scope.node->name, end_time, elapsed);
        m_active_scopes.pop_back();
    }

    void record_timeline_scope(
        const std::string&         name,
        time_point                 end_time,
        std::chrono::nanoseconds   elapsed)
    {
        const std::chrono::nanoseconds since_origin =
            end_time >= m_timeline_origin
                ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end_time - m_timeline_origin)
                : std::chrono::nanoseconds::zero();
        const auto bucket_width_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(m_timeline_bucket_width);
        const std::uint64_t bucket_index =
            bucket_width_ns.count() > 0
                ? static_cast<std::uint64_t>(since_origin.count() / bucket_width_ns.count())
                : 0U;

        if (m_timeline_buckets.size() <= bucket_index) {
            const std::size_t previous_size = m_timeline_buckets.size();
            m_timeline_buckets.resize(bucket_index + 1U);
            for (std::size_t index = previous_size;
                index < m_timeline_buckets.size();
                ++index)
            {
                m_timeline_buckets[index].index = static_cast<std::uint64_t>(index);
            }
        }

        Timeline_bucket& bucket = m_timeline_buckets[static_cast<std::size_t>(bucket_index)];
        Timeline_scope* scope = nullptr;
        for (Timeline_scope& candidate : bucket.scopes) {
            if (candidate.name == name) {
                scope = &candidate;
                break;
            }
        }
        if (scope == nullptr) {
            bucket.scopes.push_back({name});
            scope = &bucket.scopes.back();
        }

        if (scope->call_count == 0U || elapsed < scope->min_time) { scope->min_time = elapsed; }
        if (scope->call_count == 0U || elapsed > scope->max_time) { scope->max_time = elapsed; }
        ++scope->call_count;
        scope->total_time += elapsed;
    }

    static time_point default_clock()
    {
        return clock_type::now();
    }

    static Hierarchical_profiler*& active_profiler_slot()
    {
        thread_local Hierarchical_profiler* profiler = nullptr;
        return profiler;
    }

    Profile_node_snapshot snapshot_node(const Node& node, time_point snapshot_time) const
    {
        Profile_node_snapshot out;
        out.name       = node.name;
        out.call_count = node.call_count;
        out.total_time = node.total_time;
        out.min_time   = node.call_count == 0U
            ? std::chrono::nanoseconds::zero()
            : node.min_time;
        out.max_time   = node.call_count == 0U
            ? std::chrono::nanoseconds::zero()
            : node.max_time;
        for (const Active_scope& active_scope : m_active_scopes) {
            if (active_scope.node == &node) {
                record_elapsed(
                    out,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        snapshot_time - active_scope.start_time));
            }
        }
        out.children.reserve(node.children.size());
        for (const std::unique_ptr<Node>& child : node.children) {
            Profile_node_snapshot child_snapshot = snapshot_node(*child, snapshot_time);
            out.child_time += child_snapshot.total_time;
            out.children.push_back(std::move(child_snapshot));
        }
        out.self_time = out.total_time >= out.child_time
            ? out.total_time - out.child_time
            : std::chrono::nanoseconds::zero();
        return out;
    }

    static void record_elapsed(
        Profile_node_snapshot&     snapshot,
        std::chrono::nanoseconds   elapsed)
    {
        if (snapshot.call_count == 0U || elapsed < snapshot.min_time) { snapshot.min_time = elapsed; }
        if (snapshot.call_count == 0U || elapsed > snapshot.max_time) { snapshot.max_time = elapsed; }

        ++snapshot.call_count;
        snapshot.total_time += elapsed;
    }

    static std::string_view normalized_scope_name(std::string_view name)
    {
        return name.empty() ? k_profile_unnamed_scope_name : name;
    }

    static Node& child_for_name(Node& parent, std::string_view name)
    {
        const std::string_view normalized_name = normalized_scope_name(name);
        for (const std::unique_ptr<Node>& child : parent.children) {
            if (child->name == normalized_name) {
                return *child;
            }
        }

        auto child  = std::make_unique<Node>();
        child->name = std::string(normalized_name);
        Node& node = *child;
        parent.children.push_back(std::move(child));
        return node;
    }

    Node                           m_root;
    std::vector<Active_scope>      m_active_scopes;
    std::vector<Timeline_bucket>   m_timeline_buckets;
    clock_function                 m_clock;
    time_point                     m_timeline_origin = m_clock();
    std::chrono::milliseconds      m_timeline_bucket_width{100};
    mutable std::mutex             m_mutex;
    std::uint64_t                  m_generation      = 1U;
    std::uint64_t                  m_next_scope_id   = 0U;
};

class Active_profiler_binding
{
public:
    explicit Active_profiler_binding(Hierarchical_profiler* profiler)
    :
        m_previous(Hierarchical_profiler::bind_active_profiler(profiler))
    {}

    ~Active_profiler_binding()
    {
        Hierarchical_profiler::bind_active_profiler(m_previous);
    }

    Active_profiler_binding(const Active_profiler_binding&)            = delete;
    Active_profiler_binding& operator=(const Active_profiler_binding&) = delete;

private:
    Hierarchical_profiler* m_previous = nullptr;
};

class Profile_scope
{
public:
    explicit Profile_scope(std::string_view name)
    :
        m_profiler(Hierarchical_profiler::active_profiler())
    {
        if (m_profiler == nullptr) {
            return;
        }

        m_token = m_profiler->begin_scope(name);
    }

    ~Profile_scope()
    {
        if (m_profiler != nullptr) {
            m_profiler->end_scope(m_token);
        }
    }

    Profile_scope(const Profile_scope&)            = delete;
    Profile_scope& operator=(const Profile_scope&) = delete;

private:
    Hierarchical_profiler*                m_profiler = nullptr;
    Hierarchical_profiler::scope_token_t  m_token;
};

}

#define VNM_TERMINAL_PROFILE_SCOPE_JOIN_2(a, b) a##b
#define VNM_TERMINAL_PROFILE_SCOPE_JOIN(a, b) VNM_TERMINAL_PROFILE_SCOPE_JOIN_2(a, b)
#if defined(__COUNTER__)
#define VNM_TERMINAL_PROFILE_SCOPE_UNIQUE_NAME(prefix) \
    VNM_TERMINAL_PROFILE_SCOPE_JOIN(prefix, __COUNTER__)
#else
#define VNM_TERMINAL_PROFILE_SCOPE_UNIQUE_NAME(prefix) \
    VNM_TERMINAL_PROFILE_SCOPE_JOIN(prefix, __LINE__)
#endif

#if !defined(VNM_TERMINAL_PROFILING_ENABLED)
#define VNM_TERMINAL_PROFILING_ENABLED 0
#endif

#if VNM_TERMINAL_PROFILING_ENABLED
#define VNM_TERMINAL_PROFILE_SCOPE(name)    \
    ::vnm_terminal::internal::Profile_scope \
    VNM_TERMINAL_PROFILE_SCOPE_UNIQUE_NAME(vnm_terminal_profile_scope_)(name)
#else
#define VNM_TERMINAL_PROFILE_SCOPE(name) static_cast<void>(0)
#endif
