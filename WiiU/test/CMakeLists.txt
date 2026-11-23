# Wii U hardware test for the PPSSPP port: checks the toolchain pieces the port relies on
# (CafeGLSL, TLS in an RPX, 64-bit atomics, little-endian types) and draws with GX2.
cmake_minimum_required(VERSION 3.13)
project(ppsspp_wiiu_test C CXX)

set(CMAKE_CXX_STANDARD 20)
set(PPSSPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(cafeglsl REQUIRED)

add_executable(ppsspp_wiiu_test main.cpp ${PPSSPP_ROOT}/Common/WiiU/Atomic64.c)
target_include_directories(ppsspp_wiiu_test PRIVATE ${PPSSPP_ROOT})
target_compile_options(ppsspp_wiiu_test PRIVATE -Wall)
target_link_libraries(ppsspp_wiiu_test PRIVATE cafeglsl::cafeglsl)

wut_create_rpx(ppsspp_wiiu_test)
wut_create_wuhb(ppsspp_wiiu_test
	NAME "PPSSPP Wii U test"
	SHORTNAME "PPSSPP test"
	AUTHOR "vbarros22")
