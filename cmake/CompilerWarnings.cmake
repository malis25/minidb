function(minidb_set_compiler_warnings target)
    if(MSVC)
        # /W4 enables useful diagnostics; /permissive- asks MSVC to follow standard C++ rules.
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        # Use equivalent warning levels with GCC and Clang.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
