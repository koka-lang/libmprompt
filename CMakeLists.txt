# -----------------------------------------------------------------------------
# Copyright (c) 2021, Microsoft Research, Daan Leijen
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.12)
project(libmprompt C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

option(MP_USE_C             "Build C versions of the library without exception support" OFF)
option(MP_DEBUG_UBSAN       "Build with undefined behaviour sanitizer" OFF)
option(MP_DEBUG_ASAN        "Build with address sanitizer" OFF)

set(mp_version "0.6")

# all sources are included in one file so we can generate independent libraries and stand-alone object files.
set(mprompt_sources  src/mprompt/main.c)
    # util.c gstack_pool.c gstack_win.c gstack_mmap.c gstack_mmap_mach.c gstack.c mprompt.c

set(mpeff_sources    src/mpeff/main.c)
    # src/mpeff/mpeff.c

set(test_mpe_main_sources
    test/common_util.c
    test/common_effects.c
    test/src/reader.c
    test/src/counter.c
    test/src/countern.c
    test/src/mstate.c
    test/src/amb.c
    test/src/amb_state.c
    test/src/nqueens.c
    test/src/rehandle.c
    test/test_mpe_main.c)    

if (NOT MP_USE_C)
list(APPEND test_mpe_main_sources
    test/src/exn.cpp
    test/src/multi_unwind.cpp
    test/src/throw.cpp)
endif()

set(test_mp_async_sources 
    test/test_mp_async.c
    test/common_util.c)

set(test_mp_example_generator_sources 
    test/test_mp_example_generator.c)

set(test_mp_example_async_sources 
    test/test_mp_example_async.c)


list(APPEND test_sources 
      ${test_mpe_main_sources}  
      ${test_mp_async_sources} 
      ${test_mp_example_generator_sources}
      ${test_mp_example_async_sources})

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
  if ("${CMAKE_BINARY_DIR}" MATCHES ".*(Debug|debug|ubsan|tsan|asan)$")
    message(STATUS "No build type selected, default to: Debug")
    set(CMAKE_BUILD_TYPE "Debug")
  else()
    message(STATUS "No build type selected, default to: Release")
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()


# -----------------------------------------------------------------------------
# Sanitizers
# -----------------------------------------------------------------------------

if(MP_DEBUG_UBSAN OR MP_DEBUG_ASAN)
  if(NOT MP_USE_C AND (CMAKE_BUILD_TYPE MATCHES "Debug") AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(mp_san)
    if (MP_DEBUG_UBSAN)
      list(APPEND mp_san "undefined")
    endif()
    if (MP_DEBUG_ASAN)
      list(APPEND mp_san "address")
    endif()
    list(JOIN mp_san "," mp_san)
    list(APPEND mp_cflags -fsanitize=${mp_san})
    list(APPEND CMAKE_EXE_LINKER_FLAGS -fsanitize=${mp_san})
  else()
    message(WARNING "Can only use sanitizer with a clang++ debug build (currently: ${CMAKE_CXX_COMPILER_ID}, CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}, MP_USE_C=${MP_USE_C})")
  endif()
endif()


# -----------------------------------------------------------------------------
# Flags
# -----------------------------------------------------------------------------
if((CMAKE_BUILD_TYPE MATCHES "Release") OR ((CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo") AND NOT APPLE))
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

set(GNULIKE $<OR:$<C_COMPILER_ID:AppleClang>,$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Intel>>)
if(GNULIKE)
  list(APPEND mp_cflags -Wall -Wextra -Wno-unknown-pragmas -fvisibility=hidden)
endif()

if(APPLE)
  # list(APPEND mp_cflags -fasynchronous-unwind-tables)
endif()

# treat C extension as C++
if (NOT MP_USE_C)
  if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    list(APPEND mp_cflags -Wno-deprecated)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Intel")
    list(APPEND mp_cflags -Kc++)
  endif()
endif()



# -----------------------------------------------------------------------------
# Overview
# -----------------------------------------------------------------------------

message(STATUS "")
message(STATUS   "Libraries : lib${mp_mprompt_name}, lib${mp_mpeff_name}")
message(STATUS   "Build type: ${CMAKE_BUILD_TYPE}")
if(MP_USE_C)
  message(STATUS "Compiler  : ${CMAKE_C_COMPILER}")
else()
  message(STATUS "Compiler  : ${CMAKE_CXX_COMPILER}")
endif()
message(STATUS   "            ${mp_cflags}")
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



#---------------------------------------------------------------
# tests
#---------------------------------------------------------------
add_executable(test_mpe_main              ${test_mpe_main_sources})
add_executable(test_mp_async              ${test_mp_async_sources})
add_executable(test_mp_example_generator  ${test_mp_example_generator_sources})
add_executable(test_mp_example_async      ${test_mp_example_async_sources})

set(test_targets test_mpe_main test_mp_async test_mp_example_generator test_mp_example_async)


# finalize tests
enable_testing()
foreach(test_target ${test_targets} )
  target_compile_options(${test_target} PRIVATE ${mp_cflags} $<${GNULIKE}:-Wno-extra>)
  target_include_directories(${test_target} PRIVATE include test)
  target_link_libraries(${test_target} PRIVATE mpeff)
  add_test( ${test_target} ${test_target})
endforeach()
