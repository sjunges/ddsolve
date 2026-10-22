cmake_minimum_required(VERSION 3.18)
project(ddsolve)

# temporary
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -g")
# temporary

if(NOT SKBUILD AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  message(STATUS "Setting install prefix to binary dir")
  set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE ${CMAKE_CURRENT_BINARY_DIR})
endif()

find_package(Python 3.12 COMPONENTS Interpreter Development.Module REQUIRED)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanobind" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/lace" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/sylvan" EXCLUDE_FROM_ALL)
set_property(TARGET sylvan PROPERTY POSITION_INDEPENDENT_CODE TRUE)
set_property(TARGET lace PROPERTY POSITION_INDEPENDENT_CODE TRUE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(GMP REQUIRED)

set(SMT_SOLVER "CVC5" CACHE STRING "Either CVC5 or Z3.")
set_property(CACHE SMT_SOLVER PROPERTY STRINGS "CVC5" "Z3")

if(SMT_SOLVER STREQUAL "CVC5")
  message(STATUS "Using cvc5")
  find_package(cvc5 REQUIRED)
  set(SMT_LIBS cvc5::cvc5 cvc5::cvc5parser)
elseif(SMT_SOLVER STREQUAL "Z3")
  message(STATUS "Using z3")
  find_package(Z3 REQUIRED)
  set(SMT_LIBS ${Z3_LIBRARIES})
else()
  message(FATAL_ERROR "SMT Solver ${SMT_SOLVER} unsupported!")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(xxhash REQUIRED IMPORTED_TARGET libxxhash)


add_library(ddsolve_obj OBJECT src/smt/smt.cpp src/XADD.cpp src/sylvan/sylvan_ddsolve.c src/sylvan/SylvanInterface.cpp src/smt/SmtTerm.cpp)
target_link_libraries(ddsolve_obj PUBLIC sylvan::sylvan ${GMPXX_LIBRARIES} ${GMP_LIBRARIES} PkgConfig::xxhash ${SMT_LIBS})
target_include_directories(ddsolve_obj PUBLIC src/ ${GMP_INCLUDE_DIR} ${GMPXX_INCLUDE_DIR})
set_property(TARGET ddsolve_obj PROPERTY CXX_STANDARD 23)
set_property(TARGET ddsolve_obj PROPERTY POSITION_INDEPENDENT_CODE TRUE)

nanobind_add_module(_ddsolve NOMINSIZE $<TARGET_OBJECTS:ddsolve_obj> src/main.cpp)
nanobind_add_stub(_ddsolve_stub MODULE _ddsolve OUTPUT _ddsolve.pyi PYTHON_PATH "." DEPENDS _ddsolve)

if(NOT SKBUILD)
    add_custom_command(TARGET _ddsolve_stub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_BINARY_DIR}/_ddsolve.pyi
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ddsolve/_ddsolve.pyi
    )
endif()
target_link_libraries(_ddsolve PUBLIC sylvan::sylvan ${GMPXX_LIBRARIES} ${GMP_LIBRARIES} PkgConfig::xxhash ${SMT_LIBS})
target_include_directories(_ddsolve PUBLIC src/ ${GMP_INCLUDE_DIR} ${GMPXX_INCLUDE_DIR})
set_property(TARGET _ddsolve PROPERTY CXX_STANDARD 23)

#set_property(TARGET _ddsolve PROPERTY LINK_FLAGS -ltcmalloc)
#set_property(TARGET _ddsolve PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#set_property(TARGET ddsolve_obj PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

#target_compile_options(ddsolve_obj PRIVATE $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>: -fconcepts-diagnostics-depth=10>)
target_compile_options(_ddsolve PRIVATE -fno-omit-frame-pointer)
target_compile_options(ddsolve_obj PRIVATE -fno-omit-frame-pointer)
target_compile_options(lace PRIVATE -fno-omit-frame-pointer)
target_compile_options(sylvan PRIVATE -fno-omit-frame-pointer)


option(MEASURE_SMT "Measure time spent in SMT solver" OFF)
set(MEASURE_SMT_MAX_THREADS "128" CACHE STRING "Maximal number of lace workers + 1")

option(USE_SMTTERM_CACHE "Utilize a hashcache to speedup cvc5 term parsing." OFF)
option(USE_INCREMENTAL_SMT "Utilize overlap between SMT querries." ON)
option(SMT_DEBUG "Print SMT querries.")
option(DO_PRUNE "Utilize non trivial SMT pruning." ON)
configure_file(src/smt/smt_config.hpp.in smt/smt_config.hpp)

option(NO_SCALE_CACHE "Don't use the cache")
option(NO_MINMAX_GATHER_CACHE "Don't use the cache")
option(NO_MINMAX_TRANSFORM_CACHE "Don't use the cache")
option(REAL_PRUNEING "Use QF_LRA to decide if a state should be pruned.")
configure_file(src/sylvan/sylvan_ddsolve_config.h.in sylvan/sylvan_ddsolve_config.h)

# For the configured headers
target_include_directories(ddsolve_obj PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

if(SKBUILD EQUAL 2)
  message(STATUS "Building with scikit-build-core.")
endif()

install(DIRECTORY src/ddsolve/ DESTINATION ddsolve)
install(TARGETS _ddsolve LIBRARY DESTINATION ddsolve)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/_ddsolve.pyi DESTINATION ddsolve)
