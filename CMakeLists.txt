cmake_minimum_required(VERSION 3.5)
project(fpvLiberator)

include(FindPkgConfig)
pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

include(CTest)
enable_testing()

set (CMAKE_CXX_STANDARD 11)

file(GLOB SOURCES "src/*.cpp" )
file(GLOB HEADERS "src/*.h" )

add_executable(${PROJECT_NAME}  ${SOURCES} ${HEADERS})
target_link_libraries(${PROJECT_NAME} PkgConfig::libusb)

# MOBILE_MODE=ON  -> target DJI mobile/MFI mode (2ca3:1002), the mode with video.
# MOBILE_MODE=OFF -> target PC mode (2ca3:0020), diagnostics only (no video).
option(MOBILE_MODE "Target DJI mobile/MFI mode (2ca3:1002) instead of PC mode" ON)
if(MOBILE_MODE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FPV_MOBILE_MODE=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE FPV_MOBILE_MODE=0)
endif()
