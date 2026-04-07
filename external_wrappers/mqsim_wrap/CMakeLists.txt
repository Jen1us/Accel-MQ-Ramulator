cmake_minimum_required(VERSION 3.10)

project(mqsim_wrap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mqsim_wrap SHARED
  mqsim_wrap.cpp
)

# Compile MQSim sources directly into this wrapper so accel-sim can dlopen a
# single shared object without linking the whole simulator.
file(GLOB_RECURSE MQSIM_SRC
  "${CMAKE_CURRENT_LIST_DIR}/../../MQSim/src/*.cpp"
)
list(FILTER MQSIM_SRC EXCLUDE REGEX "/main\\.cpp$")
target_sources(mqsim_wrap PRIVATE ${MQSIM_SRC})

target_include_directories(mqsim_wrap PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../MQSim/src"
)

target_link_libraries(mqsim_wrap PRIVATE pthread)

