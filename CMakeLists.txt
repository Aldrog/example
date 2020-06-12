project(example)

cmake_minimum_required(VERSION 3.14)
set(CMAKE_CXX_STANDARD 17)

find_package(amqpcpp REQUIRED)

add_executable(${PROJECT_NAME}
               test.cpp
              )
target_link_libraries(${PROJECT_NAME} amqpcpp pthread)
