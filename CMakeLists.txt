cmake_minimum_required(VERSION 3.16)
project(sfodbc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

set(SOURCES
  src/util.cpp src/config.cpp src/http.cpp src/salesforce.cpp src/metadata.cpp
  src/cursor.cpp src/sql_parser.cpp src/sql_plan.cpp src/convert.cpp
  src/catalog.cpp src/info.cpp src/odbc_impl.cpp src/odbc_exports.cpp)

if(WIN32)
  list(APPEND SOURCES src/sfodbc.def)
endif()

add_library(sfodbc SHARED ${SOURCES})
target_link_libraries(sfodbc PRIVATE CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(sfodbc PRIVATE odbc32 odbccp32)
  target_compile_definitions(sfodbc PRIVATE _CRT_SECURE_NO_WARNINGS)
  set_target_properties(sfodbc PROPERTIES OUTPUT_NAME "EloquixSalesforceODBC")
else()
  find_path(ODBC_INCLUDE_DIR sql.h PATHS /usr/include /usr/local/include /opt/homebrew/include)
  find_library(ODBCINST_LIB NAMES odbcinst iodbcinst PATHS /usr/lib /usr/local/lib /opt/homebrew/lib)
  if(NOT ODBCINST_LIB)
    message(FATAL_ERROR "odbcinst library not found (install unixodbc-dev)")
  endif()
  target_include_directories(sfodbc PRIVATE ${ODBC_INCLUDE_DIR})
  # Link only odbcinst (DSN lookup), never the driver manager itself.
  target_link_libraries(sfodbc PRIVATE ${ODBCINST_LIB})
  target_compile_options(sfodbc PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(NOT APPLE)
    # Bind internal SQL* calls to our own symbols, not the driver manager's.
    target_link_options(sfodbc PRIVATE -Wl,-Bsymbolic -Wl,--no-undefined)
  endif()
endif()

install(TARGETS sfodbc LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
