#include "task.hh"

td::container::detail::CallableWrapper::~CallableWrapper() = default;

void td::container::detail::FuncPtrWrapper::call() { func_ptr(userdata); }

static_assert(sizeof(td::container::Task) == td::system::l1_cacheline_size, "Task exceeds cacheline size");

// is_trivial_v is too strong since the ctor has a default init
// the valid / invalid can be entirely removed if strict constraints are utilized
static_assert(std::is_trivially_destructible_v<td::container::Task>, "Task not trivial");
static_assert(std::is_trivially_copyable_v<td::container::Task>, "Task not trivial");
static_assert(std::is_trivially_copy_assignable_v<td::container::Task>, "Task not trivial");
static_assert(std::is_trivially_move_assignable_v<td::container::Task>, "Task not trivial");
