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
static_assert(td::l1_cacheline_size == std::hardware_destructive_interference_size, "L1 Cacheline size assumption wrong");
#else
// Clang doesn't support std::hardware_destructive_interference yet
#endif

uint32_t td::getNumLogicalCPUCores() noexcept { return std::thread::hardware_concurrency(); }

uint32_t td::getNumPhysicalCPUCores() noexcept
{
#ifdef CC_OS_WINDOWS
    std::byte buffer_stack[4096];
    cc::array<std::byte> buffer_heap;
    DWORD buffer_length = CC_COUNTOF(buffer_stack);

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer_data = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(&buffer_stack[0]);

    if (!GetLogicalProcessorInformationEx(RelationAll, buffer_data, &buffer_length))
    {
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            // stack buffer insufficient, allocate
            buffer_heap = cc::array<std::byte>::uninitialized(buffer_length);
            buffer_data = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer_heap.data());

            if (!GetLogicalProcessorInformationEx(RelationAll, buffer_data, &buffer_length))
            {
                // unexpected
                return 0;
            }
        }
        else
        {
            // other error
            return 0;
        }
    }

    uint32_t res_num_cores = 0u;
    uint32_t prev_processor_info_size = 0u;
    std::byte const* cursor = reinterpret_cast<std::byte*>(buffer_data);
    uint32_t cursor_offset = 0u;
    while (cursor_offset < buffer_length)
    {
        cursor += prev_processor_info_size;
        auto const* const info_struct = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX const*>(cursor);

        if (info_struct->Relationship == RelationProcessorCore)
            ++res_num_cores;

        cursor_offset += info_struct->Size;
        prev_processor_info_size = info_struct->Size;
    }

    return res_num_cores;
#elif defined(CC_OS_LINUX)
    // WARNING: This is Intel only and also does not account for whether hyperthreading is actually enabled, only whether it's possible on the CPU
    // do not use
    uint32_t registers[4];
    __asm__ __volatile__("cpuid " : "=a"(registers[0]), "=b"(registers[1]), "=c"(registers[2]), "=d"(registers[3]) : "a"(1), "c"(0));

    auto const feature_set_flags = registers[3];
    bool const has_hyperthreading = feature_set_flags & (1 << 28);

    if (has_hyperthreading)
        return getNumLogicalCPUCores() / 2;
    else
        return getNumLogicalCPUCores();
#else
#error "Unsupported operating system"
#endif
}
