#include "system_info.hh"

#include <new>
#include <thread>

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS
#include <clean-core/array.hh>
#include <clean-core/native/win32_sanitized.hh>
#elif defined(CC_OS_LINUX)
#include <unistd.h>
#endif

#ifdef CC_COMPILER_MSVC
static_assert(td::system::l1_cacheline_size == std::hardware_destructive_interference_size, "L1 Cacheline size assumption wrong");
#else
// Clang doesn't support std::hardware_destructive_interference yet
#endif

unsigned td::system::num_logical_cores() noexcept { return std::thread::hardware_concurrency(); }

unsigned td::system::num_physical_cores() noexcept
{
#ifdef CC_OS_WINDOWS
    cc::array<std::byte> buffer;
    auto const to_ptr = [](std::byte* raw) { return reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(raw); };
    ::DWORD length = 0;

    while (true)
    {
        auto const rc = ::GetLogicalProcessorInformationEx(RelationAll, to_ptr(buffer.data()), &length);

        if (rc)
            break;

        if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            buffer = buffer.uninitialized(length);
        else
            return 0;
    }

    auto res_num_cores = 0u;

    auto prev_processor_info_size = 0u;
    std::byte* cursor = buffer.data();
    auto cursor_offset = 0u;
    while (cursor_offset < length)
    {
        cursor += prev_processor_info_size;
        auto const* const info_struct = to_ptr(cursor);

        if (info_struct->Relationship == RelationProcessorCore)
            ++res_num_cores;

        cursor_offset += info_struct->Size;
        prev_processor_info_size = info_struct->Size;
    }

    return res_num_cores;
#elif defined(CC_OS_LINUX)
    uint32_t registers[4];
    __asm__ __volatile__("cpuid " : "=a"(registers[0]), "=b"(registers[1]), "=c"(registers[2]), "=d"(registers[3]) : "a"(1), "c"(0));

    auto const feature_set_flags = registers[3];
    bool const has_hyperthreading = feature_set_flags & (1 << 28);

    if (has_hyperthreading)
        return num_logical_cores() / 2;
    else
        return num_logical_cores();
#else
#error "Unsupported operating system"
#endif
}
