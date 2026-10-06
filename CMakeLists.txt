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

option(STEAMVITA_HELPER_ONLY "Build only the SteamVita updater helper" OFF)

set(VITA_APP_NAME "SteamVita")
set(VITA_TITLEID  "STMVITA01")
set(VITA_VERSION  "00.13")
set(UPDATER_TITLEID "STMVUPD01")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -mword-relocations -fno-optimize-sibling-calls")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -mword-relocations -fno-optimize-sibling-calls")

set(MINIZ_SOURCES
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
)

foreach(MINIZ_SOURCE IN LISTS MINIZ_SOURCES)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${MINIZ_SOURCE}")
    message(FATAL_ERROR "miniz is missing; run the CI dependency step.")
  endif()
endforeach()

if(STEAMVITA_HELPER_ONLY)
  add_executable(SteamVitaUpdater
    src/updater_main.cpp
    src/vpk_install.cpp
    ${MINIZ_SOURCES}
  )

  target_include_directories(SteamVitaUpdater PRIVATE
    src
    third_party/miniz
  )

  target_compile_definitions(SteamVitaUpdater PRIVATE
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    MINIZ_NO_ARCHIVE_WRITING_APIS
  )

  target_link_options(SteamVitaUpdater PRIVATE
    "LINKER:-z,max-page-size=0x10000"
    "LINKER:--defsym=__sce_headroom=0x10000"
  )

  target_link_libraries(SteamVitaUpdater
    mbedtls
    mbedx509
    mbedcrypto
    vita2d
    freetype
    png
    jpeg
    z
    m
    c
    SceDisplay_stub
    SceGxm_stub
    SceCtrl_stub
    SceSysmodule_stub
    ScePgf_stub
    SceAppMgr_stub
    ScePromoterUtil_stub
    SceIofilemgr_stub
    SceCommonDialog_stub
    SceKernelThreadMgr_stub
  )

  vita_create_self(updater_eboot.bin SteamVitaUpdater UNSAFE)

  vita_create_vpk(SteamVitaUpdater.vpk ${UPDATER_TITLEID} updater_eboot.bin
    VERSION ${VITA_VERSION}
    NAME "SteamVita Updater"
    FILE sce_sys/icon0.png sce_sys/icon0.png
         sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
         sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
         sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )

  return()
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/qrcodegen/c/qrcodegen.c")
  message(FATAL_ERROR "qrcodegen is missing; run the CI dependency step.")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/SteamVitaUpdater.vpk")
  message(FATAL_ERROR "SteamVitaUpdater.vpk is missing; build the helper first.")
endif()

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/steam_client.cpp
  src/compat_runtime.cpp
  src/x86_runtime.cpp
  src/game_installer.cpp
  src/update_manager.cpp
  src/vpk_install.cpp
  third_party/qrcodegen/c/qrcodegen.c
  ${MINIZ_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  third_party/qrcodegen/c
  third_party/miniz
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES
  MINIZ_NO_ARCHIVE_WRITING_APIS
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
  ScePromoterUtil_stub
  SceIofilemgr_stub
  SceCommonDialog_stub
  SceIme_stub
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
       assets/SteamVitaUpdater.vpk updater/SteamVitaUpdater.vpk
)
