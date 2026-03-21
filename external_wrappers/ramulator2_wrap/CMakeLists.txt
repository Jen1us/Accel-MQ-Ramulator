cmake_minimum_required(VERSION 3.17)

project(ramulator2_wrap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ramulator2_wrap SHARED
  ramulator2_wrap.cpp
)

# Compile Ramulator2 sources directly into this wrapper to avoid FetchContent
# (network may be unavailable in the execution environment).
file(GLOB_RECURSE RAM2_SRC
  "${CMAKE_CURRENT_LIST_DIR}/../../ramulator2/src/*.cpp"
)
list(FILTER RAM2_SRC EXCLUDE REGEX "/main\\.cpp$")
# Some sources exist in-tree but are not part of Ramulator2's default build and
# may not compile against the current interfaces.
list(FILTER RAM2_SRC EXCLUDE REGEX "/dram_controller/impl/plugin/trr\\.cpp$")
list(FILTER RAM2_SRC EXCLUDE REGEX "/example/")
target_sources(ramulator2_wrap PRIVATE ${RAM2_SRC})

target_include_directories(ramulator2_wrap PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../ramulator2/src"
  "${CMAKE_CURRENT_LIST_DIR}/../../ramulator2"
)

# Prefer the user's conda prefix if available (spdlog/fmt/yaml-cpp are often
# available there even when internet access is restricted).
set(CONDA_PREFIX "$ENV{CONDA_PREFIX}")
if (NOT CONDA_PREFIX)
  set(CONDA_PREFIX "/home/jenius/miniconda3")
endif()
if (EXISTS "${CONDA_PREFIX}/include")
  target_include_directories(ramulator2_wrap PRIVATE "${CONDA_PREFIX}/include")
endif()
if (EXISTS "${CONDA_PREFIX}/lib")
  target_link_directories(ramulator2_wrap PRIVATE "${CONDA_PREFIX}/lib")
  # Make dlopen() of libramulator2_wrap.so work without requiring users to
  # manually extend LD_LIBRARY_PATH for conda-provided deps (fmt/spdlog/yaml-cpp).
  set_target_properties(ramulator2_wrap PROPERTIES BUILD_RPATH "${CONDA_PREFIX}/lib")
endif()

target_link_libraries(ramulator2_wrap PRIVATE spdlog fmt yaml-cpp pthread)

# spdlog is typically built against external fmt in conda.
target_compile_definitions(ramulator2_wrap PRIVATE SPDLOG_FMT_EXTERNAL)
