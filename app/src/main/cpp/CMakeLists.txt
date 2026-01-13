cmake_minimum_required(VERSION 3.22.1)
project(native_lib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(native-lib SHARED
        native-lib.cpp)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(glesv3-lib GLESv3)

target_link_libraries(native-lib
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${glesv3-lib})

target_link_options(native-lib PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384")
