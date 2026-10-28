# Black (original Xbox), statically recompiled: libmain.so on Android, loaded by SDL's activity, and an executable on macOS.
# The x86 code is recompiled first (build.bat, scripts/build-apple.sh): this build takes generated/game as it is, with
# the SDK's portable runtime, the game's sources in src and the host in native.

cmake_minimum_required(VERSION 3.22)
project(black_android LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT ANDROID AND NOT APPLE)
    message(FATAL_ERROR "This build is for Android and Apple hosts")
endif()
# unoptimized recompiled code is too slow to run
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(REPO ${CMAKE_CURRENT_SOURCE_DIR})
set(NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/native)
set(GAME ${CMAKE_CURRENT_SOURCE_DIR}/src)  # the game's hooks, sound, Direct3D 8 and settings

# the SDK: sdk/ in the repo, a checkout beside it, else downloaded
include(FetchContent)
if(EXISTS ${REPO}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${REPO}/sdk)
elseif(EXISTS ${REPO}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    get_filename_component(SDK ${REPO}/../XboxXBE-Recomp ABSOLUTE)
else()
    # pinned: the SDK's main moved to game code built into a DLL at first start (Windows), which this port's
    # recompiler step and libmain.so do not take; this is the commit v1.5.0 was built with
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git
                         GIT_TAG 99078b119b5d5e0d1d6afb1390e29437c0c984c0)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()

# the recompiled code and the xbe it came from
set(XBE_GENERATED ${REPO}/generated/game CACHE PATH "the recompiled code of default.xbe")
set(XBE_IMAGE ${REPO}/assets/default.xbe CACHE FILEPATH "the xbe it was recompiled from")
set(GEN ${XBE_GENERATED})
set(XBE ${XBE_IMAGE})
if(NOT EXISTS ${GEN}/x86_func_mapping.cpp OR NOT EXISTS ${XBE})
    message(FATAL_ERROR "No recompiled code: put the game in assets/ or iso/ and run build.bat, it writes generated/game")
endif()
file(STRINGS ${REPO}/config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
math(EXPR last "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
foreach(i RANGE ${last})
    list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
endforeach()
file(SHA1 ${XBE} xbe_sha1)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
# src/common/europe.patch as a byte array for game_files.cpp: .incbin is ELF assembly, this builds on every host
set(EUROPE_PATCH_FILE ${GAME}/common/europe.patch)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EUROPE_PATCH_FILE})
file(READ ${EUROPE_PATCH_FILE} europe_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," europe_bytes "${europe_hex}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/europe_patch.cpp CONTENT
"#include <cstdint>\nextern \"C\" {\nextern const uint8_t kEuropePatch[];\nextern const uint8_t* const kEuropePatchEnd;\nconst uint8_t kEuropePatch[] = {${europe_bytes}};\nconst uint8_t* const kEuropePatchEnd = kEuropePatch + sizeof(kEuropePatch);\n}\n"
ESCAPE_QUOTES)
# the build's version for the log: the commit the tree is at when the build runs (cmake/build_info.cmake), not
# when cmake configured; created now so the first configure has the file, refreshed by a target before every build
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
execute_process(COMMAND ${CMAKE_COMMAND} -DREPO=${REPO} -DOUT=${BUILD_INFO} -P ${REPO}/cmake/build_info.cmake)
add_custom_target(build_info ALL COMMAND ${CMAKE_COMMAND} -DREPO=${REPO} -DOUT=${BUILD_INFO}
                  -P ${REPO}/cmake/build_info.cmake BYPRODUCTS ${BUILD_INFO})

# dependencies: SDL3 from the checkout Gradle makes (its Java side goes into the APK), the rest fetched
set(SDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL)
if(NOT EXISTS ${SDL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "SDL3 is missing from ${SDL_DIR}: the Gradle build clones it")
endif()
if(ANDROID)
    # the activity loads libSDL3.so
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 16.0.0 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glslang tomlplusplus imgui)

# the SDK's runtime but its Windows parts, and those the game replaces (src/sound, src/d3d)
file(GLOB RUNTIME_SOURCES ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|render_thread)\\.cpp$|/audio/xaudio2\\.cpp$|/xdk/(dsound|d3d8)\\.cpp$")
# the game's sources: its hooks, sound, Direct3D 8 and settings
file(GLOB GAME_HOST_SOURCES CONFIGURE_DEPENDS ${GAME}/*/*.cpp)
# the host: native/*.cpp but the platform parts, of which the build takes its own
file(GLOB ANDROID_SOURCES CONFIGURE_DEPENDS ${NATIVE}/*.cpp)
list(FILTER ANDROID_SOURCES EXCLUDE REGEX "/platform_[a-z]+\\.cpp$")
if(ANDROID)
    list(APPEND ANDROID_SOURCES ${NATIVE}/platform_android.cpp)
else()
    list(APPEND ANDROID_SOURCES ${NATIVE}/platform_apple.cpp ${NATIVE}/platform_apple_objc.mm)
endif()
set(IMGUI_SOURCES ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)

set(MAIN_SOURCES ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${ANDROID_SOURCES} ${GEN_SOURCES} ${IMGUI_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp ${CMAKE_CURRENT_BINARY_DIR}/europe_patch.cpp)
if(ANDROID)
    add_library(main SHARED ${MAIN_SOURCES})
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # an app bundle: Info.plist from ios/, the touch skin in its resources; signed afterwards by scripts/sign-ios.sh
    set(SKIN_PNG ${REPO}/app/src/main/assets/skins/buttons.png)
    set_source_files_properties(${SKIN_PNG} PROPERTIES MACOSX_PACKAGE_LOCATION skins)
    # the app icon: 1024 px, from the cover art the project already uses, compiled by actool
    set(ICON_CATALOG ${REPO}/ios/Assets.xcassets)
    set_source_files_properties(${ICON_CATALOG} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # the HD texture map, read from the bundle
    set(HD_MAP ${REPO}/textures/black-hd-reimagined.map)
    set_source_files_properties(${HD_MAP} PROPERTIES MACOSX_PACKAGE_LOCATION textures)
    add_executable(main MACOSX_BUNDLE ${MAIN_SOURCES} ${SKIN_PNG} ${ICON_CATALOG} ${HD_MAP})
    set_target_properties(main PROPERTIES
        OUTPUT_NAME Black
        MACOSX_BUNDLE_INFO_PLIST ${REPO}/ios/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER org.black.ios
        MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
        MACOSX_BUNDLE_BUNDLE_VERSION 1
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 1)
else()
    add_executable(main ${MAIN_SOURCES})
    set_target_properties(main PROPERTIES OUTPUT_NAME black)
endif()
add_dependencies(main build_info)
# compat/ for the SDK's x86 header, the SDK harness's for the Winsock and DXGI names its portable runtime uses
target_include_directories(main PRIVATE ${NATIVE}/compat ${SDK}/harness/compat ${NATIVE} ${GAME} ${SDK}/runtime ${GEN}
                           ${CMAKE_CURRENT_BINARY_DIR} ${glslang_SOURCE_DIR})
target_include_directories(main SYSTEM PRIVATE ${imgui_SOURCE_DIR})
# x86 chars are signed, as the game's code and the runtime were written for
target_compile_options(main PRIVATE -fno-strict-aliasing -fsigned-char)
if(APPLE)
    set_source_files_properties(${NATIVE}/platform_apple_objc.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
endif()
set(HOST_SOURCES ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${ANDROID_SOURCES})
option(BLACK_VERBOSE_LOG "info and warning lines in the log, as a debug build writes them (the SDK's logx follows NDEBUG)" OFF)
set(HOST_OPTIONS -include ${NATIVE}/compat/crt.h)
if(BLACK_VERBOSE_LOG)
    list(APPEND HOST_OPTIONS -UNDEBUG)
endif()
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "${HOST_OPTIONS}")
set_source_files_properties(${GEN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing;-fsigned-char")
set_source_files_properties(${NATIVE}/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
target_link_libraries(main PRIVATE SDL3::SDL3 tomlplusplus::tomlplusplus glslang glslang-default-resource-limits)
if(ANDROID)
    target_link_libraries(main PRIVATE vulkan android log)
    # 16 KB pages, as Android 15 devices may have
    target_link_options(main PRIVATE -Wl,-z,max-page-size=16384)
else()
    # MoltenVK linked in, not loaded: SDL finds vkGetInstanceProcAddr in the executable. It is only pulled from the
    # archive if something asks for it, hence -u
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # the official prebuilt package, scripts/fetch-deps.sh ios
        set(MVK_IOS ${REPO}/third_party/MoltenVK-ios/MoltenVK/MoltenVK)
        set(MOLTENVK_LIB ${MVK_IOS}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a)
        set(VULKAN_HEADERS ${MVK_IOS}/include)
        if(NOT EXISTS ${MOLTENVK_LIB})
            message(FATAL_ERROR "MoltenVK for iOS is missing: run scripts/fetch-deps.sh ios")
        endif()
    else()
        set(MOLTENVK_PREFIX "" CACHE PATH "where MoltenVK's lib and the Vulkan headers are (Homebrew's molten-vk and vulkan-headers)")
        find_library(MOLTENVK_LIB NAMES libMoltenVK.a MoltenVK HINTS ${MOLTENVK_PREFIX}/lib /opt/homebrew/opt/molten-vk/lib
                     /usr/local/opt/molten-vk/lib REQUIRED)
        find_path(VULKAN_HEADERS vulkan/vulkan.h HINTS ${MOLTENVK_PREFIX}/include /opt/homebrew/opt/vulkan-headers/include
                  /usr/local/opt/vulkan-headers/include REQUIRED)
    endif()
    target_include_directories(main SYSTEM PRIVATE ${VULKAN_HEADERS})
    target_link_libraries(main PRIVATE ${MOLTENVK_LIB} "-framework Metal" "-framework QuartzCore" "-framework IOSurface"
                          "-framework Foundation" "-framework CoreGraphics" "-framework GameController")
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(main PRIVATE "-framework UIKit" "-framework AVFAudio")
    else()
        target_link_libraries(main PRIVATE "-framework IOKit" "-framework AppKit")
    endif()
    target_link_options(main PRIVATE -Wl,-u,_vkGetInstanceProcAddr)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # the touch controls' skin next to the executable
        add_custom_command(TARGET main POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
                           ${REPO}/app/src/main/assets/skins $<TARGET_FILE_DIR:main>/skins)
        add_custom_command(TARGET main POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy
                           ${REPO}/textures/black-hd-reimagined.map $<TARGET_FILE_DIR:main>/textures/black-hd-reimagined.map)
    endif()
endif()
