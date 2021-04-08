# -----------------------------------------------------------------------------
# Copyright (c) 2021, Microsoft Research, Daan Leijen
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.9)
project(libmimalloc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

option(MP_USE_C             "Build C versions of the library without exception support" OFF)
set(mp_version "0.4")

# all sources are included in one file so we can generate independent libraries and stand-alone object files.
set(mprompt_sources  src/mprompt/main.c)
    # util.c gstack_pool.c gstack_win.c gstack_mmap.c gstack_mmap_mach.c gstack.c mprompt.c

set(mpeff_sources    src/mpeff/main.c)
    # src/mpeff/mpeff.c

set(test_main_sources
    test/util.c
    test/common_effects.c
    test/src/reader.c
    test/src/counter.c
    test/src/countern.c
    test/src/mstate.c
    test/src/amb.c
    test/src/amb_state.c
    test/src/nqueens.c
    test/src/rehandle.c
    test/test_main.c)    

if (MP_USE_C)
set(test_mainx_sources)
else()
set(test_mainx_sources
    test/util.c
    test/common_effects.c
    test/src/exn.cpp
    test/src/multi_unwind.cpp
    test/src/rehandlex.cpp test/src/reader.c test/src/amb_state.c
    test/src/throw.cpp
    test/test_mainx.cpp)  
endif()

set(test_async_sources 
    test/test_async.c)


list(APPEND test_sources ${test_main_sources} ${test_mainx_sources} ${test_async_sources})

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
    set(mprompt_asm_source src/mprompt/asm/longjmp_amd64.S)
  endif()
else()
  if (mp_proc MATCHES "(aarch64)|(arm64)")
    if (APPLE)
      message(WARNING "arm64 support on Apple is untested")
    endif()
    set(mprompt_asm_source src/mprompt/asm/longjmp_arm64.S)
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
if((CMAKE_BUILD_TYPE MATCHES "Release") OR ((CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo") AND NOT APPLE))
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

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
add_executable(test_main  ${test_main_sources})
add_executable(test_async ${test_async_sources})

set(test_targets test_main test_async)

# C++ tests
if (NOT MP_USE_C)
add_executable(test_mainx ${test_mainx_sources})
list(APPEND test_targets test_mainx)
endif()

# finalize tests
enable_testing()
foreach(test_target ${test_targets} )
  target_compile_options(${test_target} PRIVATE ${mp_cflags} -Wno-extra)
  target_include_directories(${test_target} PRIVATE include test)
  target_link_libraries(${test_target} PRIVATE mpeff)
  add_test( ${test_target} ${test_target})
endforeach()
