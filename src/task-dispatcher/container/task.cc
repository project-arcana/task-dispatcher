#include "task.hh"

td::container::detail::callable_wrapper::~callable_wrapper() = default;

void td::container::detail::func_ptr_wrapper::call() { _func_ptr(_userdata); }

static_assert(sizeof(td::container::task) == td::system::l1_cacheline_size, "task exceeds cacheline size");
static_assert(std::is_trivial_v<td::container::task>, "task is not trivial");
