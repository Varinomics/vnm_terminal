#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "helpers/test_check.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

term::Hierarchical_profiler::time_point test_time;

static_assert(!std::is_copy_constructible_v<term::Hierarchical_profiler>);
static_assert(!std::is_copy_assignable_v<term::Hierarchical_profiler>);
static_assert(!std::is_move_constructible_v<term::Hierarchical_profiler>);
static_assert(!std::is_move_assignable_v<term::Hierarchical_profiler>);

using vnm_terminal::test_helpers::check;

term::Hierarchical_profiler::time_point test_clock()
{
    return test_time;
}

void set_time(std::chrono::nanoseconds time)
{
    test_time = term::Hierarchical_profiler::time_point(time);
}

void advance_time(std::chrono::nanoseconds delta)
{
    test_time += delta;
}

term::Hierarchical_profiler make_test_profiler()
{
    return term::Hierarchical_profiler(test_clock);
}

constexpr std::chrono::nanoseconds ns(std::int64_t value)
{
    return std::chrono::nanoseconds(value);
}

const term::Profile_node_snapshot* child_named(
    const std::vector<term::Profile_node_snapshot>&
                           nodes,
    std::string_view       name)
{
    for (const term::Profile_node_snapshot& node : nodes) {
        if (node.name == name) {
            return &node;
        }
    }

    return nullptr;
}

const term::Profile_timeline_scope_snapshot* timeline_scope_named(
    const std::vector<term::Profile_timeline_scope_snapshot>&
                           scopes,
    std::string_view       name)
{
    for (const term::Profile_timeline_scope_snapshot& scope : scopes) {
        if (scope.name == name) {
            return &scope;
        }
    }

    return nullptr;
}

bool test_inactive_scope_noop()
{
    bool ok = true;

    term::Active_profiler_binding inactive_binding(nullptr);
    {
        term::Profile_scope scope("inactive");
        VNM_TERMINAL_PROFILE_SCOPE("inactive-macro");
    }

    ok &= check(term::Hierarchical_profiler::active_profiler() == nullptr,
        "inactive scope keeps active profiler empty");
    return ok;
}

bool test_manual_begin_end()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(100));
    profiler.begin_scope("manual");
    advance_time(ns(17));
    profiler.end_scope();

    const term::Profile_node_snapshot  root   = profiler.root_snapshot();
    const term::Profile_node_snapshot* manual = child_named(root.children, "manual");
    ok &= check(root.name == term::k_profile_root_scope_name, "root snapshot name");
    ok &= check(root.call_count == 0U, "root snapshot is not a scope call");
    ok &= check(root.total_time == ns(17), "root snapshot total time");
    ok &= check(root.self_time == ns(0), "root snapshot self time");
    ok &= check(root.child_time == ns(17), "root snapshot child time");
    ok &= check(root.min_time == ns(0), "root snapshot min time");
    ok &= check(root.max_time == ns(0), "root snapshot max time");
    ok &= check(root.children.size() == 1U, "manual scope creates one root child");
    ok &= check(manual != nullptr, "manual scope node is present");
    if (manual != nullptr) {
        ok &= check(manual->call_count == 1U, "manual scope count");
        ok &= check(manual->total_time == ns(17), "manual scope nanosecond total");
        ok &= check(manual->self_time == ns(17), "manual scope self time");
        ok &= check(manual->child_time == ns(0), "manual scope child time");
        ok &= check(manual->min_time == ns(17), "manual scope min time");
        ok &= check(manual->max_time == ns(17), "manual scope max time");
        ok &= check(manual->children.empty(), "manual scope has no children");
    }

    return ok;
}

bool test_unmatched_end_scope_is_noop()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));

    profiler.end_scope();
    ok &= check(profiler.snapshot().empty(), "unmatched end scope leaves snapshot empty");

    profiler.begin_scope("after-unmatched-end");
    advance_time(ns(9));
    profiler.end_scope();

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* node =
        child_named(snapshot, "after-unmatched-end");
    ok &= check(node != nullptr, "profiler records after unmatched end");
    if (node != nullptr) {
        ok &= check(node->call_count == 1U, "post-unmatched-end count");
        ok &= check(node->total_time == ns(9), "post-unmatched-end total");
    }

    return ok;
}

bool test_nested_scopes()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    profiler.begin_scope("outer");
    advance_time(ns(5));
    profiler.begin_scope("inner");
    advance_time(ns(7));
    profiler.end_scope();
    advance_time(ns(3));
    profiler.end_scope();

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* outer = child_named(snapshot, "outer");
    ok &= check(outer != nullptr, "nested outer node is present");
    if (outer != nullptr) {
        const term::Profile_node_snapshot* inner = child_named(outer->children, "inner");
        ok &= check(outer->call_count == 1U, "nested outer count");
        ok &= check(outer->total_time == ns(15), "nested outer inclusive total");
        ok &= check(outer->child_time == ns(7), "nested outer child time");
        ok &= check(outer->self_time == ns(8), "nested outer self time");
        ok &= check(outer->min_time == ns(15), "nested outer min time");
        ok &= check(outer->max_time == ns(15), "nested outer max time");
        ok &= check(inner != nullptr, "nested inner node is present");
        if (inner != nullptr) {
            ok &= check(inner->call_count == 1U, "nested inner count");
            ok &= check(inner->total_time == ns(7), "nested inner total");
            ok &= check(inner->child_time == ns(0), "nested inner child time");
            ok &= check(inner->self_time == ns(7), "nested inner self time");
            ok &= check(inner->min_time == ns(7), "nested inner min time");
            ok &= check(inner->max_time == ns(7), "nested inner max time");
        }
    }

    return ok;
}

bool test_snapshot_includes_open_outer_scope()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    profiler.begin_scope("outer");
    advance_time(ns(5));
    profiler.begin_scope("inner");
    advance_time(ns(7));
    profiler.end_scope();
    advance_time(ns(3));

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* outer = child_named(snapshot, "outer");
    ok &= check(outer != nullptr, "open outer node is present");
    if (outer != nullptr) {
        const term::Profile_node_snapshot* inner = child_named(outer->children, "inner");
        ok &= check(outer->call_count == 1U, "open outer in-progress count");
        ok &= check(outer->total_time == ns(15), "open outer in-progress total");
        ok &= check(outer->child_time == ns(7), "open outer child time");
        ok &= check(outer->self_time == ns(8), "open outer self time");
        ok &= check(outer->min_time == ns(15), "open outer min time");
        ok &= check(outer->max_time == ns(15), "open outer max time");
        ok &= check(inner != nullptr, "closed child under open outer is present");
        if (inner != nullptr) {
            ok &= check(inner->call_count == 1U, "closed child count under open outer");
            ok &= check(inner->total_time == ns(7), "closed child total under open outer");
        }
    }

    advance_time(ns(5));
    profiler.end_scope();
    const std::vector<term::Profile_node_snapshot> final_snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* final_outer =
        child_named(final_snapshot, "outer");
    ok &= check(final_outer != nullptr, "closed outer node is present after open snapshot");
    if (final_outer != nullptr) {
        ok &= check(final_outer->call_count == 1U,
            "open snapshot does not mutate outer count");
        ok &= check(final_outer->total_time == ns(20),
            "outer final total comes from real end");
    }

    return ok;
}

bool test_snapshot_includes_nested_open_scopes()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    profiler.begin_scope("outer");
    advance_time(ns(4));
    profiler.begin_scope("inner");
    advance_time(ns(6));

    const term::Profile_node_snapshot  root  = profiler.root_snapshot();
    const term::Profile_node_snapshot* outer = child_named(root.children, "outer");
    ok &= check(root.total_time == ns(10), "open nested root total");
    ok &= check(root.child_time == ns(10), "open nested root child time");
    ok &= check(outer != nullptr, "open nested outer node is present");
    if (outer != nullptr) {
        const term::Profile_node_snapshot* inner = child_named(outer->children, "inner");
        ok &= check(outer->call_count == 1U, "open nested outer count");
        ok &= check(outer->total_time == ns(10), "open nested outer total");
        ok &= check(outer->child_time == ns(6), "open nested outer child time");
        ok &= check(outer->self_time == ns(4), "open nested outer self time");
        ok &= check(outer->min_time == ns(10), "open nested outer min time");
        ok &= check(outer->max_time == ns(10), "open nested outer max time");
        ok &= check(inner != nullptr, "open nested inner node is present");
        if (inner != nullptr) {
            ok &= check(inner->call_count == 1U, "open nested inner count");
            ok &= check(inner->total_time == ns(6), "open nested inner total");
            ok &= check(inner->child_time == ns(0), "open nested inner child time");
            ok &= check(inner->self_time == ns(6), "open nested inner self time");
            ok &= check(inner->min_time == ns(6), "open nested inner min time");
            ok &= check(inner->max_time == ns(6), "open nested inner max time");
        }
    }

    profiler.end_scope();
    profiler.end_scope();
    return ok;
}

bool test_repeated_aggregation()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    profiler.begin_scope("repeat");
    advance_time(ns(11));
    profiler.end_scope();

    advance_time(ns(2));
    profiler.begin_scope("repeat");
    advance_time(ns(13));
    profiler.end_scope();

    profiler.begin_scope("parent");
    profiler.begin_scope("child");
    advance_time(ns(3));
    profiler.end_scope();
    profiler.begin_scope("child");
    advance_time(ns(5));
    profiler.end_scope();
    profiler.end_scope();

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot*             repeat   = child_named(snapshot, "repeat");
    const term::Profile_node_snapshot*             parent   = child_named(snapshot, "parent");
    ok &= check(repeat != nullptr, "repeated root node is present");
    if (repeat != nullptr) {
        ok &= check(repeat->call_count == 2U, "repeated root count aggregates");
        ok &= check(repeat->total_time == ns(24), "repeated root time aggregates");
        ok &= check(repeat->min_time == ns(11), "repeated root min time");
        ok &= check(repeat->max_time == ns(13), "repeated root max time");
    }
    ok &= check(parent != nullptr, "parent node is present");
    if (parent != nullptr) {
        const term::Profile_node_snapshot* child = child_named(parent->children, "child");
        ok &= check(child != nullptr, "repeated child node is present");
        if (child != nullptr) {
            ok &= check(child->call_count == 2U, "repeated child count aggregates");
            ok &= check(child->total_time == ns(8), "repeated child time aggregates");
            ok &= check(child->min_time == ns(3), "repeated child min time");
            ok &= check(child->max_time == ns(5), "repeated child max time");
        }
        ok &= check(parent->child_time == ns(8), "repeated parent child time");
        ok &= check(parent->self_time == ns(0), "repeated parent self time");
    }

    return ok;
}

bool test_reset()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    profiler.begin_scope("before-reset");
    advance_time(ns(19));
    profiler.end_scope();
    ok &= check(!profiler.snapshot().empty(), "reset fixture starts with data");

    profiler.reset();
    ok &= check(profiler.snapshot().empty(), "reset clears recorded nodes");
    const term::Profile_node_snapshot empty_root = profiler.root_snapshot();
    ok &= check(empty_root.name == term::k_profile_root_scope_name, "reset preserves root name");
    ok &= check(empty_root.total_time == ns(0), "reset clears root total time");
    ok &= check(empty_root.children.empty(), "reset clears root children");

    profiler.begin_scope("after-reset");
    advance_time(ns(23));
    profiler.end_scope();
    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* after_reset =
        child_named(snapshot, "after-reset");
    ok &= check(after_reset != nullptr, "profiler records after reset");
    if (after_reset != nullptr) {
        ok &= check(after_reset->call_count == 1U, "post-reset count");
        ok &= check(after_reset->total_time == ns(23), "post-reset time");
    }

    return ok;
}

bool test_reset_drops_live_scope()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    {
        term::Active_profiler_binding binding(&profiler);
        {
            term::Profile_scope live_scope("dropped-live");
            advance_time(ns(12));
            profiler.reset();
            ok &= check(profiler.snapshot().empty(), "reset with live scope clears data");
        }
    }

    profiler.begin_scope("post-reset");
    advance_time(ns(5));
    profiler.end_scope();

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* post_reset =
        child_named(snapshot, "post-reset");
    ok &= check(snapshot.size() == 1U, "reset with live scope leaves one post-reset node");
    ok &= check(post_reset != nullptr, "post-reset node after live reset is present");
    ok &= check(child_named(snapshot, "dropped-live") == nullptr,
        "reset drops live scope node");
    if (post_reset != nullptr) {
        ok &= check(post_reset->call_count == 1U, "post-reset after live reset count");
        ok &= check(post_reset->total_time == ns(5), "post-reset after live reset total");
    }

    return ok;
}

bool test_reset_stale_scope_does_not_close_new_scope()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    term::Active_profiler_binding binding(&profiler);
    set_time(ns(0));

    auto stale_scope = std::make_unique<term::Profile_scope>("stale");
    advance_time(ns(3));
    profiler.reset();

    term::Profile_scope new_scope("new-active");
    advance_time(ns(5));
    stale_scope.reset();
    advance_time(ns(7));

    const std::vector<term::Profile_node_snapshot> open_snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* open_node =
        child_named(open_snapshot, "new-active");
    ok &= check(open_node != nullptr, "stale destructor leaves new active node present");
    if (open_node != nullptr) {
        ok &= check(open_node->call_count == 1U, "new active node remains open");
        ok &= check(open_node->total_time == ns(12), "new active node keeps elapsed time");
    }

    return ok;
}

bool test_empty_scope_name_normalization()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    profiler.begin_scope("");
    advance_time(ns(6));
    profiler.end_scope();
    {
        term::Active_profiler_binding binding(&profiler);
        {
            term::Profile_scope scope("");
            advance_time(ns(8));
        }
    }

    const term::Profile_node_snapshot root = profiler.root_snapshot();
    const term::Profile_node_snapshot* unnamed =
        child_named(root.children, term::k_profile_unnamed_scope_name);
    ok &= check(unnamed != nullptr, "empty scope name is normalized");
    if (unnamed != nullptr) {
        ok &= check(!unnamed->name.empty(), "normalized scope name is not empty");
        ok &= check(unnamed->call_count == 2U, "normalized scope count aggregates");
        ok &= check(unnamed->total_time == ns(14), "normalized scope total");
        ok &= check(unnamed->min_time == ns(6), "normalized scope min time");
        ok &= check(unnamed->max_time == ns(8), "normalized scope max time");
    }

    return ok;
}

bool test_binding_restore_and_thread_locality()
{
    bool ok = true;

    term::Hierarchical_profiler first  = make_test_profiler();
    term::Hierarchical_profiler second = make_test_profiler();
    {
        term::Active_profiler_binding first_binding(&first);
        ok &= check(term::Hierarchical_profiler::active_profiler() == &first,
            "first binding is active");
        {
            term::Active_profiler_binding second_binding(&second);
            ok &= check(term::Hierarchical_profiler::active_profiler() == &second,
                "nested binding is active");
        }
        ok &= check(term::Hierarchical_profiler::active_profiler() == &first,
            "nested binding restores previous profiler");

        bool worker_ok = true;
        std::thread worker([&worker_ok] {
            worker_ok &= check(term::Hierarchical_profiler::active_profiler() == nullptr,
                "worker thread starts without main-thread profiler");
        });
        worker.join();
        ok &= worker_ok;
    }

    ok &= check(term::Hierarchical_profiler::active_profiler() == nullptr,
        "outer binding restores null profiler");
    return ok;
}

bool test_raii_scope_behavior()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    {
        term::Active_profiler_binding binding(&profiler);
        {
            term::Profile_scope scope("raii");
            advance_time(ns(31));
        }
    }

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* raii = child_named(snapshot, "raii");
    ok &= check(raii != nullptr, "RAII scope node is present");
    if (raii != nullptr) {
        ok &= check(raii->call_count == 1U, "RAII scope count");
        ok &= check(raii->total_time == ns(31), "RAII scope nanosecond total");
    }

    return ok;
}

bool test_same_line_macro_uniqueness()
{
    bool ok = true;

#if defined(__COUNTER__)
    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    {
        term::Active_profiler_binding binding(&profiler);
        {
            VNM_TERMINAL_PROFILE_SCOPE("same-a"); VNM_TERMINAL_PROFILE_SCOPE("same-b"); advance_time(ns(37));
        }
    }

    const term::Profile_node_snapshot  root   = profiler.root_snapshot();
    const term::Profile_node_snapshot* parent = child_named(root.children, "same-a");
    ok &= check(parent != nullptr, "same-line macro parent node is present");
    if (parent != nullptr) {
        const term::Profile_node_snapshot* child =
            child_named(parent->children, "same-b");
        ok &= check(parent->total_time == ns(37), "same-line macro parent total");
        ok &= check(child != nullptr, "same-line macro child node is present");
        if (child != nullptr) {
            ok &= check(child->total_time == ns(37), "same-line macro child total");
        }
    }
#endif

    return ok;
}

bool test_raii_macro_behavior()
{
    bool ok = true;

    term::Hierarchical_profiler profiler = make_test_profiler();
    set_time(ns(0));
    {
        term::Active_profiler_binding binding(&profiler);
        {
            VNM_TERMINAL_PROFILE_SCOPE("macro");
            advance_time(ns(29));
        }
    }

    const std::vector<term::Profile_node_snapshot> snapshot = profiler.snapshot();
    const term::Profile_node_snapshot* macro = child_named(snapshot, "macro");
    ok &= check(macro != nullptr, "macro scope node is present");
    if (macro != nullptr) {
        ok &= check(macro->call_count == 1U, "macro scope count");
        ok &= check(macro->total_time == ns(29), "macro scope nanosecond total");
    }

    return ok;
}

bool test_timeline_buckets_scope_activity()
{
    bool ok = true;

    set_time(ns(0));
    term::Hierarchical_profiler profiler = make_test_profiler();
    profiler.begin_scope("burst");
    advance_time(std::chrono::milliseconds(10));
    profiler.end_scope();

    set_time(std::chrono::milliseconds(150));
    profiler.begin_scope("burst");
    advance_time(std::chrono::milliseconds(25));
    profiler.end_scope();

    profiler.begin_scope("other");
    advance_time(std::chrono::milliseconds(5));
    profiler.end_scope();

    const term::Profile_timeline_snapshot timeline = profiler.timeline_snapshot();
    ok &= check(timeline.bucket_width == std::chrono::milliseconds(100),
        "timeline bucket width");
    ok &= check(timeline.buckets.size() == 2U, "timeline keeps non-empty buckets");
    if (timeline.buckets.size() >= 2U) {
        const term::Profile_timeline_bucket_snapshot& first  = timeline.buckets[0];
        const term::Profile_timeline_bucket_snapshot& second = timeline.buckets[1];
        const term::Profile_timeline_scope_snapshot* first_burst =
            timeline_scope_named(first.scopes, "burst");
        const term::Profile_timeline_scope_snapshot* second_burst =
            timeline_scope_named(second.scopes, "burst");
        const term::Profile_timeline_scope_snapshot* second_other =
            timeline_scope_named(second.scopes, "other");
        ok &= check(first.start_time == std::chrono::milliseconds(0),
            "first timeline bucket start");
        ok &= check(first.end_time == std::chrono::milliseconds(100),
            "first timeline bucket end");
        ok &= check(second.start_time == std::chrono::milliseconds(100),
            "second timeline bucket start");
        ok &= check(second.end_time == std::chrono::milliseconds(200),
            "second timeline bucket end");
        ok &= check(first_burst != nullptr, "first timeline bucket records burst");
        if (first_burst != nullptr) {
            ok &= check(first_burst->call_count == 1U, "first burst call count");
            ok &= check(first_burst->total_time == std::chrono::milliseconds(10),
                "first burst total");
            ok &= check(first_burst->max_time == std::chrono::milliseconds(10),
                "first burst max");
        }
        ok &= check(second_burst != nullptr, "second timeline bucket records burst");
        if (second_burst != nullptr) {
            ok &= check(second_burst->call_count == 1U, "second burst call count");
            ok &= check(second_burst->total_time == std::chrono::milliseconds(25),
                "second burst total");
        }
        ok &= check(second_other != nullptr, "second timeline bucket records other");
        if (second_other != nullptr) {
            ok &= check(second_other->call_count == 1U, "second other call count");
            ok &= check(second_other->total_time == std::chrono::milliseconds(5),
                "second other total");
        }
    }

    profiler.reset();
    ok &= check(profiler.timeline_snapshot().buckets.empty(), "reset clears timeline");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_inactive_scope_noop();
    ok &= test_manual_begin_end();
    ok &= test_unmatched_end_scope_is_noop();
    ok &= test_nested_scopes();
    ok &= test_snapshot_includes_open_outer_scope();
    ok &= test_snapshot_includes_nested_open_scopes();
    ok &= test_repeated_aggregation();
    ok &= test_reset();
    ok &= test_reset_drops_live_scope();
    ok &= test_reset_stale_scope_does_not_close_new_scope();
    ok &= test_empty_scope_name_normalization();
    ok &= test_binding_restore_and_thread_locality();
    ok &= test_raii_scope_behavior();
    ok &= test_same_line_macro_uniqueness();
    ok &= test_raii_macro_behavior();
    ok &= test_timeline_buckets_scope_activity();
    return ok ? 0 : 1;
}
