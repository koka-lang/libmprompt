# -----------------------------------------------------------------------------
# Copyright (c) 2021, Microsoft Research, Daan Leijen
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.9)
project(libmimalloc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

option(MP_USE_C  "Build C versions of the library without exception support" OFF)

set(mp_version "0.1")

# all sources are included in one file so we can generate independent libraries and stand-alone object files.
set(mprompt_sources  src/mprompt/main.c)
    # util.c gstack-pool.c gstack-win/mmap.c gstack.c mprompt.c

set(mpeff_sources    src/mpeff/main.c)
    # src/mpeff/mpeff.c

set(test_sources
    test/util.c
    test/effects.c
    test/test-reader.c
    test/test-counter.c
    test/test-countern.c
    test/test-mstate.c
    test/test-amb.c
    test/test-amb-state.c
    test/test-nqueens.c
    test/test-rehandle.c
    test/main.c)    

set(mp_cflags)
set(mp_install_dir)

# -----------------------------------------------------------------------------
# C or C++ (default)
# -----------------------------------------------------------------------------
if (MP_USE_C)
  # C
  message(STATUS "Use the C compiler to compile (MP_USE_C=ON)")  
  set(mp_mprompt_name "mprompt")
  set(mp_mpeff_name   "mpeff") 

  if(CMAKE_C_COMPILER_ID MATCHES "MSVC|Intel")
    message(WARNING "It is not recommended to use plain C with this compiler (due to SEH) (${CMAKE_C_COMPILER_ID})")
  endif() 
else()  
  # C++
  message(STATUS "Use the C++ compiler to compile (${CMAKE_CXX_COMPILER_ID}) (MP_USE_C=OFF)")  
  set(mp_mprompt_name "mpromptx")
  set(mp_mpeff_name   "mpeffx")
    
  list(APPEND test_sources 
              test/test-exn.cpp
              test/test-multi-unwind.cpp
              test/test-throw.cpp)

  SET_SOURCE_FILES_PROPERTIES(${mprompt_sources} PROPERTIES LANGUAGE CXX )
  SET_SOURCE_FILES_PROPERTIES(${mpeff_sources} PROPERTIES LANGUAGE CXX )
  SET_SOURCE_FILES_PROPERTIES(${test_sources} PROPERTIES LANGUAGE CXX )
  
  if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    list(APPEND mp_cflags -Wno-deprecated)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Intel")
    list(APPEND mp_cflags -Kc++)
  endif()
endif()


# -----------------------------------------------------------------------------
# Check architecture: currently only amd64 and arm64 (experimental)
# -----------------------------------------------------------------------------
set(mprompt_asm_source)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" mp_proc)
if (mp_proc MATCHES "(x86_64)|(amd64)")
  if (CMAKE_C_COMPILER_ID MATCHES "MSVC")
    set(mprompt_asm_source src/mprompt/asm/longjmp_amd64_win.asm)
    enable_language(ASM_MASM)
  else()
    set(mprompt_asm_source src/mprompt/asm/longjmp_amd64.s)
  endif()
else()
  if (mp_proc MATCHES "(aarch64)|(arm64)")
    message(WARNING "arm64 support is experimental")
    set(mprompt_asm_source src/mprompt/asm/longjmp_arm64.s)
  else()
    message(WARNING "unsupported architecture: ${mp_proc}")    
  endif()
endif()


# -----------------------------------------------------------------------------
# Convenience: set default build type depending on the build directory
# -----------------------------------------------------------------------------

if (NOT CMAKE_BUILD_TYPE)
  if ("${CMAKE_BINARY_DIR}" MATCHES ".*(D|d)ebug$")
    message(STATUS "No build type selected, default to: Debug")
    set(CMAKE_BUILD_TYPE "Debug")
  else()
    message(STATUS "No build type selected, default to: Release")
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()


# -----------------------------------------------------------------------------
# Flags
# -----------------------------------------------------------------------------
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
  list(APPEND mp_cflags -Wall -Wextra -Wno-unknown-pragmas -fvisibility=hidden)
endif()

if(APPLE)
  # list(APPEND mp_cflags -fasynchronous-unwind-tables)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "MSVC")
  list(APPEND mp_cflags -EHa)   # needed for reliable calling of destructors
endif()

if (CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU|Intel|ARMClang|ARMCC")
  set_source_files_properties(${mprompt_asm_source} PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp")
endif()


message(STATUS "")
message(STATUS "Libraries : lib${mp_mprompt_name}, lib${mp_mpeff_name}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(MP_USE_C)
  message(STATUS "Compiler  : ${CMAKE_C_COMPILER} ${mp_cflags}")
else()
  message(STATUS "Compiler  : ${CMAKE_CXX_COMPILER} ${mp_cflags}")
endif()
message(STATUS "")


# -----------------------------------------------------------------------------
# Libraries
# Each library is standalone; mphnd includes mprompt, while mpeff includes mphnd (and mprompt)
# -----------------------------------------------------------------------------

add_library(mprompt STATIC ${mprompt_sources} ${mprompt_asm_source})
# set_property(TARGET mprompt PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(mprompt PROPERTIES VERSION ${mp_version} OUTPUT_NAME ${mp_mprompt_name} )
target_compile_definitions(mprompt PRIVATE MP_STATIC_LIB)
target_compile_options(mprompt PRIVATE ${mp_cflags})
target_include_directories(mprompt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${mp_install_dir}/include>
)
if (NOT WIN32)
  target_link_libraries(mprompt PUBLIC pthread)
endif()
if (APPLE)  
  # target_link_options(mprompt PUBLIC -Wl,-no_compact_unwind -Wl,-keep_dwarf_unwind)
  # target_link_options(mprompt PUBLIC -Wl,-no_keep_dwarf_unwind)
endif()


# mpeff library
add_library(mpeff STATIC ${mpeff_sources} ${mprompt_asm_source})
set_target_properties(mpeff PROPERTIES VERSION ${mp_version} OUTPUT_NAME ${mp_mpeff_name} )
target_compile_definitions(mpeff PRIVATE MPE_STATIC_LIB)
target_compile_options(mpeff PRIVATE ${mp_cflags})
target_include_directories(mpeff PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${mp_install_dir}/include>
)
if (NOT WIN32)
  target_link_libraries(mpeff PUBLIC pthread)
endif()

# tests
add_executable(mptest ${test_sources})
target_compile_options(mptest PRIVATE ${mp_cflags})
target_include_directories(mptest PRIVATE include)
target_link_libraries(mptest PRIVATE mpeff)

enable_testing()
add_test(test_api, mptest)