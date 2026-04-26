cmake_minimum_required(VERSION 3.20)

project(libdatachannel_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
include(FetchContent)

if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
  find_program(BREW_EXECUTABLE brew)
  if(BREW_EXECUTABLE)
    execute_process(
      COMMAND "${BREW_EXECUTABLE}" --prefix openssl@3
      OUTPUT_VARIABLE HOMEBREW_OPENSSL_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )

    if(HOMEBREW_OPENSSL_PREFIX)
      set(
        OPENSSL_ROOT_DIR
        "${HOMEBREW_OPENSSL_PREFIX}"
        CACHE PATH "OpenSSL installation prefix"
        FORCE
      )
    endif()
  endif()
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.2
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(libdatachannel)
FetchContent_MakeAvailable(nlohmann_json)

pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

add_executable(
  libdatachannel_demo
  src/InMemorySignaler.cpp
  src/main.cpp
)
target_link_libraries(libdatachannel_demo PRIVATE LibDataChannel::LibDataChannel)

add_library(
  p2p_demo_common
  src/Consumer.cpp
  src/Producer.cpp
  src/SignalingProtocol.cpp
  src/VideoPipeline.cpp
)
target_link_libraries(
  p2p_demo_common
  PUBLIC
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    PkgConfig::GSTREAMER
)
target_include_directories(p2p_demo_common PUBLIC src)

add_executable(producer src/producer_main.cpp)
target_link_libraries(producer PRIVATE p2p_demo_common)

add_executable(consumer src/consumer_main.cpp)
target_link_libraries(consumer PRIVATE p2p_demo_common)
