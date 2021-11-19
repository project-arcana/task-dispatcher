#pragma once

#include <type_traits> // for is_invocable, is_class, is_member_function_pointer_v
#include <utility>     // for tuple_size

#include <clean-core/apply.hh>
#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/move.hh>
#include <clean-core/tuple.hh>

#include <task-dispatcher/CounterHandle.hh>
#include <task-dispatcher/Scheduler.hh>
#include <task-dispatcher/container/Task.hh>

//
// Callable submission
//
// large header, only required for exotic callables
// prefer LambdaSubmission.hh if possible

namespace td
{
// submit a task based on any callable (lambda, funcptr, type with operator()), with arguments passed to it
// arguments are moved into the task capture immediately
template <class F, class... Args>
void submitCallable(CounterHandle counter, F&& func, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");

    // A lambda calling func(args...), moving the arguments into the capture (instead of copying)
    Task dispatch(
        [func, tup = cc::tuple(cc::move(args)...)]
        {
            // "apply" the argument tuple to go back to a parameter pack
            cc::apply(
                [&func](auto&&... args)
                {
                    // actually call the callable with expanded arguments
                    func(decltype(args)(args)...);
                },
                tup);
        });

    td::submitTasks(counter, cc::span{dispatch});
}

// submit a task based on a member function, with arguments passed to it
// arguments are moved into the task capture immediately
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
void submitMethod(CounterHandle counter, FObj* pInst, F pMemberFunc, Args&&... args)
{
    static_assert(std::is_invocable_v<F, FObj, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, FObj, Args...>, void>, "return must be void");

    // A lambda calling pInst->func(args...), moving the arguments into the capture (instead of copying)
    Task dispatch(
        [pMemberFunc, pInst, tup = cc::tuple(cc::move(args)...)]
        {
            // "apply" the argument tuple to go back to a parameter pack
            cc::apply(
                [&pMemberFunc, pInst](auto&&... args)
                {
                    // actually call the method with expanded arguments
                    (pInst->*pMemberFunc)(decltype(args)(args)...);
                },
                tup);
        });

    td::submitTasks(counter, cc::span{dispatch});
}
}
