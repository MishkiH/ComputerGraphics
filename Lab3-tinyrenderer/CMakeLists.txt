cmake_minimum_required(VERSION 3.16)

project(TinyRendererLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(tinyrenderer
    main.cpp
    model.cpp
    tgaimage.cpp
)

if(MSVC)
    target_compile_options(tinyrenderer PRIVATE /W4)
else()
    target_compile_options(tinyrenderer PRIVATE -Wall -Wextra -O2)
endif()