cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your VitaSDK path.")
  endif()
endif()

project(SteamVita LANGUAGES C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "SteamVita")
set(VITA_TITLEID  "STMVITA01")
set(VITA_VERSION  "00.02")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")

set(DOSBOX_PURE_DIR "${CMAKE_SOURCE_DIR}/third_party/dosbox-pure")
set(DOSBOX_PURE_LIB "${DOSBOX_PURE_DIR}/dosbox_pure_libretro_vita.a")

if(NOT EXISTS "${DOSBOX_PURE_LIB}")
  message(FATAL_ERROR
    "DOSBox Pure Vita library is missing. Run the CI dependency step or build "
    "third_party/dosbox-pure with: make platform=vita")
endif()

add_library(dosbox_pure STATIC IMPORTED GLOBAL)
set_target_properties(dosbox_pure PROPERTIES
  IMPORTED_LOCATION "${DOSBOX_PURE_LIB}"
)

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/library.cpp
  src/dosbox_backend.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  "${DOSBOX_PURE_DIR}/libretro-common/include"
)

target_link_libraries(${PROJECT_NAME}
  dosbox_pure
  vita2d
  freetype
  png
  jpeg
  z
  m
  c
  pthread
  SceAudio_stub
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  SceSysmodule_stub
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
)
