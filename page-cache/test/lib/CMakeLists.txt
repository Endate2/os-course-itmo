add_library(
        vt
        STATIC
        cmp_file.cpp
        exception.cpp
        file.cpp
        log_file.cpp
)

target_include_directories(vt PUBLIC .)
target_link_libraries(vt PRIVATE vtpc)

# Без этого в Release read/write могут превратиться в __read_chk и т.п.
# и уйти в ядро мимо перехвата libvtpc.so.
target_compile_options(vt PRIVATE -U_FORTIFY_SOURCE)