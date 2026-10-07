cmake_minimum_required(VERSION 3.16)
project(whyhot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_executable(whyhot
  src/main.cpp
  src/collectors/cpu.cpp
  src/collectors/process.cpp
  src/collectors/thermal.cpp
  src/collectors/fan.cpp
  src/collectors/platform.cpp
  src/correlation/engine.cpp
  src/correlation/store.cpp
  src/ui/terminal.cpp
  src/web/server.cpp
  src/web/json.cpp
)

target_include_directories(whyhot PRIVATE src)
target_link_libraries(whyhot PRIVATE Threads::Threads)

# Optional native desktop companion (Qt6 Widgets); skipped if Qt6 isn't installed.
find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
  add_executable(whyhot-desktop
    src/desktop/main.cpp
    src/collectors/cpu.cpp
    src/collectors/process.cpp
    src/collectors/thermal.cpp
    src/collectors/fan.cpp
    src/collectors/platform.cpp
    src/correlation/engine.cpp
    src/correlation/store.cpp
    src/ui/terminal.cpp
  )
  target_include_directories(whyhot-desktop PRIVATE src)
  target_link_libraries(whyhot-desktop PRIVATE Qt6::Widgets Threads::Threads)
else()
  message(STATUS "Qt6 Widgets not found - skipping whyhot-desktop")
endif()

enable_testing()
add_executable(engine_test
  tests/engine_test.cpp
  src/correlation/engine.cpp
  src/correlation/store.cpp
  src/ui/terminal.cpp
)
target_include_directories(engine_test PRIVATE src)
add_test(NAME engine_test COMMAND engine_test)
