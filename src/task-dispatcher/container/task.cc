#include "task.hh"

td::container::detail::callable_wrapper::~callable_wrapper() = default;

void td::container::detail::func_ptr_wrapper::call() { _func_ptr(_userdata); }

static_assert(sizeof(td::container::task) == td::system::l1_cacheline_size, "Task exceeds cacheline size");

// is_trivial_v is too strong since the ctor has a default init
// the valid / invalid can be entirely removed if strict constraints are utilized
static_assert(std::is_trivially_destructible_v<td::container::task>, "Task not trivial");
static_assert(std::is_trivially_copyable_v<td::container::task>, "Task not trivial");
static_assert(std::is_trivially_copy_assignable_v<td::container::task>, "Task not trivial");
static_assert(std::is_trivially_move_assignable_v<td::container::task>, "Task not trivial");
