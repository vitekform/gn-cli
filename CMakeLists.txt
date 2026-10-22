cmake_minimum_required(VERSION 3.20)
project(gn_cli VERSION 1.11.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GN_CLI_USER_AGENT_PRODUCT "gn-cli" CACHE STRING
  "HTTP User-Agent product; modified builds must use a custom name or include _fork")
if(NOT GN_CLI_USER_AGENT_PRODUCT MATCHES "^[A-Za-z0-9][A-Za-z0-9._+-]*$")
  message(FATAL_ERROR "GN_CLI_USER_AGENT_PRODUCT must be a nonempty HTTP product name (letters, digits, ., _, +, -).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# libcurl: the system one on Linux and macOS, vcpkg's (Schannel) on Windows.
find_package(CURL REQUIRED)

# nlohmann/json: the system package when there is one, otherwise fetched.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(json)
endif()

add_executable(gn-cli
  src/main.cpp
  src/term.cpp
  src/http.cpp
  src/config.cpp
  src/api.cpp
  src/commands.cpp
  src/rsys_compatibility.cpp
  src/common.cpp
  src/drop.cpp
  src/files.cpp
  src/share.cpp
  src/update.cpp
  src/lineedit.cpp
  src/chat.cpp
  src/account.cpp
  src/media.cpp
  src/breeze.cpp
  src/admin.cpp
  src/stock.cpp
  src/home.cpp
  src/kathars.cpp
  src/khk.cpp
  src/booru.cpp
  src/roleplay.cpp
  src/vault.cpp
  src/bakalink.cpp
  src/sha256.cpp
  src/license.cpp
  src/terms.cpp)

target_compile_definitions(gn-cli PRIVATE GN_CLI_VERSION="${PROJECT_VERSION}")
target_compile_definitions(gn-cli PRIVATE GN_CLI_USER_AGENT_PRODUCT="${GN_CLI_USER_AGENT_PRODUCT}")

# Embed the licence, third-party notices and a versioned copy of the service terms
# for `gn-cli license`, `gn-cli terms` and the service-terms acceptance prompt.
# The service terms ship with the source, so builds need no other repository.
# Byte arrays avoid MSVC's 16 KB string-literal limit.
set(notices_hpp "${CMAKE_CURRENT_BINARY_DIR}/generated/notices.hpp")
set(notices_content "// Generated from LICENSE, THIRD-PARTY-NOTICES and TERMS.en.md by CMakeLists.txt. Don't edit.\n#pragma once\n\n")
set(notice_files LICENSE THIRD-PARTY-NOTICES TERMS.en.md)
set(notice_names kLicense kThirdPartyNotices kTerms)
foreach(file name IN ZIP_LISTS notice_files notice_names)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
    message(FATAL_ERROR "${file} is missing: it is embedded into gn-cli. Build from a complete gn-cli source checkout.")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n    " hex "${hex}")
  string(APPEND notices_content "inline constexpr unsigned char ${name}[] = {\n    ${hex}0x00};\n\n")
endforeach()
file(CONFIGURE OUTPUT "${notices_hpp}" CONTENT "${notices_content}" @ONLY)
target_include_directories(gn-cli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(gn-cli PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)

if(MSVC)
  target_compile_options(gn-cli PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(gn-cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gn-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS gn-cli RUNTIME DESTINATION bin)

# gn and gncli are aliases: symlinks next to gn-cli, or copies where symlinks can't be made
# (Windows without developer mode).
foreach(alias gn gncli)
  install(CODE "
    set(dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
    file(REMOVE \"\${dir}/${alias}${CMAKE_EXECUTABLE_SUFFIX}\")
    file(CREATE_LINK \"gn-cli${CMAKE_EXECUTABLE_SUFFIX}\" \"\${dir}/${alias}${CMAKE_EXECUTABLE_SUFFIX}\"
         SYMBOLIC COPY_ON_ERROR)
    message(STATUS \"Installing: \${dir}/${alias}${CMAKE_EXECUTABLE_SUFFIX} -> gn-cli${CMAKE_EXECUTABLE_SUFFIX}\")")
endforeach()
