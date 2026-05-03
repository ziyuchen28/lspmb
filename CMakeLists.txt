cmake_minimum_required(VERSION 3.20)
project(clspc LANGUAGES CXX)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# nlohmann
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# lspx (lsp client runtime and extension)
add_subdirectory(external/lspx)

# this
add_executable(lsp_mcp
  src/lsp_mcp.cpp
  src/service.cpp
  src/report.cpp
)

target_compile_features(lsp_mcp PRIVATE cxx_std_20)

target_include_directories(lsp_mcp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lsp_mcp
  PRIVATE
    lspx::lspx_jdtls
    nlohmann_json::nlohmann_json
    pcr::proc
)
