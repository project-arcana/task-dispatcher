#include "panic.hh"

#include <cstdio>
#include <cstdlib>

#if defined __unix__
#include <signal.h>
#endif

void td::detail::panic(panic_info const& info)
{
    fprintf(stderr, "Engine panic: %s\n", info.message);
    fprintf(stderr, "    in %s\n", info.function);
    fprintf(stderr, "    %s:%d\n", info.file, info.line);
    fprintf(stderr, "Panic - crashing\n");
    fflush(stderr);

    debugbreak();
    std::quick_exit(EXIT_FAILURE);
}

void td::detail::debugbreak()
{
#ifdef _WIN32
    __debugbreak();
#elif __unix__
    raise(SIGTRAP);
#endif
}
