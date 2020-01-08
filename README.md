
# task-dispatcher

High-performance intuitive task-based concurrency framework with fiber support

## Quick Start

```C++
#include <task-dispatcher/td.hh>

// launch the scheduler
td::launch([&] {
	// submit 10 tasks as lambdas
	auto sync = td::submit_n([&](unsigned task_i) {
		do_work(task_i);
	}, 10);
	
	// wait on the sync object
	td::wait_for(sync);
});

```

## Features

There are two fundamental functions, `td::submit` and `td::wait_for`, as well as one object of note, `td::sync`.

`td::sync` is a lightweight handle with which tasks are associated upon submission. The task-sync association is n:1. Submitted tasks are queued and can potentially start executing immediately.

```C++
td::sync s;
td::submit(s, []{ work(); });
td::submit(s, []{ more_work(); });
```

To ensure completion of all tasks of a sync, `td::wait_for` is used. The fiber calling it only resumes execution once all of the tasks are finished. Waiting doesn't sleep or spin, it yields to pending tasks in the meantime, with low overhead.

```C++
td::wait_for(s);
// all tasks of s are now complete
```

Syncs can be waited upon multiple times, and re-used. However, each sync ever used must be waited upon at least once, as the amount of syncs in flight is limited.

## Advanced Usage

#### Unpinned Waiting

If thread locality is not required, syncs can be waited upon in "unpinned" mode, meaning the fiber can potentially resume execution on a different OS thread. This can make scheduling more efficient.
```C++
td::wait_for_unpinned(s);
```
#### Batch Convenience

There are multiple convenience helpers for submission of multiple tasks, some of them:

```C++
// submits 50 tasks, lambdas called with indices 0 to 49
td::submit_n(s, [](unsigned i) { 
	work_on_section(i); 
}, 50);

// submits tasks for each element in the container
// lambdas called with references to the elements
std::vector<foo> elements;
td::submit_each_ref(s, [](foo& elem) {
	work_on_elem(elem);
}, elements);

// submit a large range as batches
td::submit_batched(s, [](unsigned start, unsigned end) {
	for (auto i = start; i < end; ++i) {
		work_on_section(i);
	}
}, 500);
```

#### Manual Task Management

Tasks can be managed explicitly as well, and do not require lambdas:
```C++
std::vector<td::task> tasks;
// from a lambda
tasks.push_back(td::task([&] { work(); });
// from function pointer + void* userdata
tasks.push_back(td::task(my_func, &userdata));

td::submit_raw(s, tasks);
```
The lambdas in tasks can be capturing, but the capture size is restricted. Manually managed `td::task`s require some care: If filled with a lambda, any task must be submitted no more than once. If it is never submitted, `execute_and_cleanup` must be called to ensure proper cleanup of captured objects.

#### Schedulers

The default mode of usage is to call `td::launch` once in the entire application, and then continue with everything else inside the lambda. This call is blocking until the scheduler shuts down. However, multiple schedulers are possible. 

Interaction with a scheduler is only allowed "from within", as in on a fiber that is owned by the scheduler. At any point, this can be tested by calling
```C++
td::is_scheduler_alive();
```
A scheduler can be configured using `td::scheduler_config`, passed as the first argument to `td::launch`. This mainly concerns the amount of memory consumed by the various resources which are all created up front and in fixed amounts.

#### Futures

When submitting a single lambda with a return value, a `td::future` is returned. This is a small convenience helper, encapsulating a `td::sync` and space for the return value.

```C++
td::future f = td::submit([&] { return 5; });

// calls td::wait_for on the contained sync
printf("got %d", f.get());
```