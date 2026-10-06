cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your VitaSDK path.")
  endif()
endif()

project(SteamVita LANGUAGES C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "SteamVita")
set(VITA_TITLEID  "STMVITA01")
set(VITA_VERSION  "00.05")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -mword-relocations -fno-optimize-sibling-calls")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -mword-relocations -fno-optimize-sibling-calls")

set(STEAMDEPOT_ROOT "${CMAKE_SOURCE_DIR}/third_party/libsteamdepot")
if(NOT EXISTS "${STEAMDEPOT_ROOT}/src/steam_cm.cpp")
  message(FATAL_ERROR "libsteamdepot is missing; run the CI dependency step.")
endif()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/qrcodegen/c/qrcodegen.c")
  message(FATAL_ERROR "qrcodegen is missing; run the CI dependency step.")
endif()

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/steam_client.cpp
  ${STEAMDEPOT_ROOT}/src/steam_cm.cpp
  ${STEAMDEPOT_ROOT}/src/steam_cm_types.cpp
  ${STEAMDEPOT_ROOT}/src/proto_wire.cpp
  ${STEAMDEPOT_ROOT}/src/discovery.cpp
  ${STEAMDEPOT_ROOT}/src/http_client.cpp
  ${STEAMDEPOT_ROOT}/src/secret.cpp
  third_party/qrcodegen/c/qrcodegen.c
)

target_compile_definitions(${PROJECT_NAME} PRIVATE STEAMVITA_VITA=1)

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  ${STEAMDEPOT_ROOT}/include
  ${STEAMDEPOT_ROOT}/src
  third_party/qrcodegen/c
)

target_link_options(${PROJECT_NAME} PRIVATE
  "LINKER:-z,max-page-size=0x10000"
  "LINKER:--defsym=__sce_headroom=0x10000"
)

target_link_libraries(${PROJECT_NAME}
  curl
  mbedtls
  mbedx509
  mbedcrypto
  zstd
  ssl
  crypto
  z
  vita2d
  freetype
  png
  jpeg
  m
  c
  -Wl,--whole-archive
  pthread
  -Wl,--no-whole-archive
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  ScePgf_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  SceKernelThreadMgr_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
       sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
       assets/cacert.pem cacert.pem
)
