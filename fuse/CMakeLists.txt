cmake_minimum_required(VERSION 3.16)
project(tgfs C)

set(CMAKE_C_STANDARD 11)

find_package(Td REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

add_executable(tgfs
        src/main.c
        src/fs/inode/inode.c
        src/fs/context/ctx.c
        src/fs/ops.c
        src/fs/render/render.c
        src/backend/tg_backend_tdlib.c
        ../../os-course/lab/fuse/third_party/cJSON.c
)

target_include_directories(tgfs PRIVATE
        src/backend
        src/fs
        ../../os-course/lab/fuse/third_party
)

target_compile_definitions(tgfs PRIVATE FUSE_USE_VERSION=31)

target_link_libraries(tgfs PRIVATE
        PkgConfig::FUSE3
        Td::TdJson
        Threads::Threads
)