cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME duckrouting)

# Dependencies are declared in ./vcpkg.json and resolved here. The routing
# algorithms are built on the Boost Graph Library.
find_package(boost_graph REQUIRED CONFIG)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/duckrouting_extension.cpp
    src/edges.cpp
    src/dijkstra.cpp
    src/functions.cpp
    src/yen.cpp
    src/graph_functions.cpp
    src/spanning.cpp
    src/traversal.cpp
    src/ordering.cpp
    src/analysis.cpp
    src/flow.cpp
    src/astar.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link Boost.Graph into both the static library and the loadable extension
target_link_libraries(${EXTENSION_NAME} Boost::graph)
target_link_libraries(${LOADABLE_EXTENSION_NAME} Boost::graph)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
