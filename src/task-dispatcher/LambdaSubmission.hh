#pragma once

#include <type_traits>

#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/span.hh>

#include <task-dispatcher/CounterHandle.hh>
#include <task-dispatcher/Scheduler.hh>
#include <task-dispatcher/container/Task.hh>

//
// Lambda submission
//
// slightly larger header than the raw scheduler

namespace td
{
// submit a task based on a single "void f()" lambda or function pointer
template <class F, cc::enable_if<std::is_invocable_r_v<void, F>> = true>
void submitLambda(CounterHandle counter, F&& func, ETaskPriority priority = ETaskPriority::Default)
{
    static_assert(std::is_invocable_v<F>, "td::submitLambda: Function must be callable without arguments");
    static_assert(std::is_invocable_r_v<void, F>, "td::submitLambda: Function must return void");

    Task dispatch;
    if constexpr (std::is_class_v<F>)
    {
        // proper lambda with capture
        dispatch.initWithLambda(cc::forward<F>(func));
    }
    else
    {
        // function pointer
        dispatch.initWithLambda([=] { func(); });
    }

    submitTasks(counter, cc::span{dispatch}, priority);
}

// submit a task based on a function pointer taking optional userdata
inline void submitFunction(CounterHandle counter, void (*pFunc)(void*), void* pUserdata = nullptr, ETaskPriority priority = ETaskPriority::Default)
{
    Task dispatch;
    dispatch.initWithFunction(pFunc, pUserdata);
    submitTasks(counter, cc::span{dispatch}, priority);
}
}
