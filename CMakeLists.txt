cmake_minimum_required(VERSION 3.20)

project(libdatachannel_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.2
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_MakeAvailable(libdatachannel)

add_executable(
  libdatachannel_demo
  src/InMemorySignaler.cpp
  src/main.cpp
)
target_link_libraries(libdatachannel_demo PRIVATE LibDataChannel::LibDataChannel)
