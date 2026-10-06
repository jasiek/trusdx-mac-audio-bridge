cmake_minimum_required(VERSION 3.20)

project(trusdx_mac_audio_bridge VERSION 0.3.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0) # SMAppService (Open at Login)
endif()

set(CODESIGN_ID "-" CACHE STRING "codesign identity ('-' = ad-hoc)")
set(TRUSDX_VERSION ${PROJECT_VERSION} CACHE STRING "version stamped into the bundles")

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ---- libASPL (Core Audio server plug-in framework, MIT) -------------------

include(ExternalProject)
set(LIBASPL_PREFIX ${CMAKE_BINARY_DIR}/libASPL-prefix)
set(LIBASPL_SOURCE ${CMAKE_BINARY_DIR}/libASPL-src)
# "arm64;x86_64" would be split into separate arguments; pass it '|'-separated.
string(REPLACE ";" "|" LIBASPL_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
ExternalProject_Add(libASPL_ext
  GIT_REPOSITORY https://github.com/gavv/libASPL.git
  GIT_TAG v3.1.2
  GIT_SHALLOW TRUE
  SOURCE_DIR ${LIBASPL_SOURCE}
  INSTALL_DIR ${LIBASPL_PREFIX}
  LIST_SEPARATOR |
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
    -DCMAKE_OSX_ARCHITECTURES=${LIBASPL_ARCHS}
    -DBUILD_TESTING=OFF
  BUILD_BYPRODUCTS ${LIBASPL_PREFIX}/lib/libASPL.a
  UPDATE_DISCONNECTED TRUE
  LOG_CONFIGURE TRUE
  LOG_BUILD TRUE
  LOG_INSTALL TRUE
)

find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
find_library(CORE_AUDIO CoreAudio REQUIRED)
find_library(APPKIT AppKit REQUIRED)
find_library(SERVICE_MANAGEMENT ServiceManagement REQUIRED)

# ---- truSDX.driver: the Core Audio device ---------------------------------

add_library(truSDX MODULE driver/Driver.cpp)
add_dependencies(truSDX libASPL_ext)
target_include_directories(truSDX PRIVATE common)
target_include_directories(truSDX SYSTEM PRIVATE ${LIBASPL_PREFIX}/include)
target_link_libraries(truSDX PRIVATE ${LIBASPL_PREFIX}/lib/libASPL.a ${CORE_FOUNDATION})

set(DRIVER_UID "DC52ADEE-875C-4B81-8A94-F8618BC6949A")
set(DRIVER_ENTRYPOINT "TruSDXDriverEntryPoint")
set_target_properties(truSDX PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION driver
  PREFIX ""
  SUFFIX ""
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/driver/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_NAME truSDX
  MACOSX_BUNDLE_GUI_IDENTIFIER com.github.trusdx-mac-audio-bridge.driver
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${TRUSDX_VERSION}
  MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 Jan Szumiec. Licensed under the MIT License."
)

# ---- TruSDXBridge.app: menu bar app / daemon --------------------------------

add_library(bridge_core STATIC bridge/Protocol.cpp)
target_include_directories(bridge_core PUBLIC bridge common)

add_executable(TruSDXBridge MACOSX_BUNDLE
  LICENSE
  bridge/About.txt
  bridge/Assets/AppIcon.icns
  bridge/main.mm
  bridge/Bridge.cpp
  bridge/AudioLink.cpp
  bridge/Ports.cpp
)
set_source_files_properties(bridge/About.txt bridge/Assets/AppIcon.icns LICENSE
  PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
# Resource changes must also trigger the post-build signing step.
set_property(TARGET TruSDXBridge APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/bridge/About.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/bridge/Assets/AppIcon.icns"
  "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
target_compile_options(TruSDXBridge PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(TruSDXBridge PRIVATE
  bridge_core ${CORE_AUDIO} ${CORE_FOUNDATION} ${APPKIT} ${SERVICE_MANAGEMENT})
set_target_properties(TruSDXBridge PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/bridge/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_NAME "truSDX Bridge"
  MACOSX_BUNDLE_GUI_IDENTIFIER com.github.trusdx-mac-audio-bridge.bridge
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${TRUSDX_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${TRUSDX_VERSION}
  MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 Jan Szumiec. Licensed under the MIT License."
)

# Ship the uninstaller and third-party notices inside the app (before signing).
add_custom_command(TARGET TruSDXBridge POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:TruSDXBridge>/Resources"
  COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/uninstall.sh
    "$<TARGET_BUNDLE_CONTENT_DIR:TruSDXBridge>/Resources/uninstall.sh"
  COMMAND ${CMAKE_COMMAND} -E copy
    ${LIBASPL_SOURCE}/LICENSE
    "$<TARGET_BUNDLE_CONTENT_DIR:TruSDXBridge>/Resources/libASPL-LICENSE.txt"
  VERBATIM)

foreach(target truSDX TruSDXBridge)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND codesign --force --sign "${CODESIGN_ID}" "$<TARGET_BUNDLE_DIR:${target}>"
    VERBATIM)
endforeach()

# ---- tests ------------------------------------------------------------------

enable_testing()
add_executable(trusdx_tests tests/test_main.cpp)
target_link_libraries(trusdx_tests PRIVATE bridge_core)
add_test(NAME trusdx_tests COMMAND trusdx_tests)

# Test the real serial session with controlled capture and a fake PTY radio.
add_executable(trusdx_session_tests
  tests/test_session.cpp bridge/Bridge.cpp bridge/Ports.cpp)
target_link_libraries(trusdx_session_tests PRIVATE bridge_core)
add_test(NAME trusdx_session_tests COMMAND trusdx_session_tests)
set_tests_properties(trusdx_session_tests PROPERTIES TIMEOUT 40)
