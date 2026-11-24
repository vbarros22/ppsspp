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
	NAME "PPSSPP passo a passo"
	SHORTNAME "PPSSPP passo"
	AUTHOR "vbarros22")

# Teste G: every check except the UDP log runs before the first frame.
add_executable(teste_g_antes main.cpp ${PPSSPP_ROOT}/Common/WiiU/Atomic64.c)
target_include_directories(teste_g_antes PRIVATE ${PPSSPP_ROOT})
target_compile_definitions(teste_g_antes PRIVATE CHECKS_BEFORE_FIRST_FRAME)
target_link_libraries(teste_g_antes PRIVATE cafeglsl::cafeglsl)
wut_create_rpx(teste_g_antes)
wut_create_wuhb(teste_g_antes NAME "Teste G verificacoes antes" SHORTNAME "Teste G" AUTHOR "vbarros22")

# GX2 test: one stage per press of A (see the top of gx2_test.cpp).
add_executable(ppsspp_wiiu_gx2_test gx2_test.cpp)
target_link_libraries(ppsspp_wiiu_gx2_test PRIVATE cafeglsl::cafeglsl)
wut_create_rpx(ppsspp_wiiu_gx2_test)
wut_create_wuhb(ppsspp_wiiu_gx2_test NAME "PPSSPP GX2 test" SHORTNAME "PPSSPP GX2" AUTHOR "vbarros22")
