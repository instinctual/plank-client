cmake_minimum_required(VERSION 3.30)
set(CMAKE_Swift_LANGUAGE_VERSION 6)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
project(PlankMac LANGUAGES Swift C CXX)
set(PLANK_APPLE_BUILD_NUMBER "9" CACHE STRING "Native Apple pilot build")
set(PLANK_BUILD_BRANCH "local" CACHE STRING "Source branch shown in the pilot")
set(PLANK_TRANSPORT_DIR "" CACHE PATH "Pinned plank-transport source")
set(PLANK_FFMPEG_DIR "" CACHE PATH "Target-built FFmpeg prefix")
set(PLANK_OPUS_DIR "" CACHE PATH "Pinned libopus 1.6.1 prefix")
set(PLANK_RELAY_SOURCE_DIR "" CACHE PATH "Pinned drawing Relay source")
set(PLANK_RELAY_SODIUM_PREFIX "" CACHE PATH "Target-built libsodium prefix")
set(VISION "${CMAKE_CURRENT_SOURCE_DIR}/../visionos-native")
foreach(required PLANK_TRANSPORT_DIR PLANK_FFMPEG_DIR PLANK_OPUS_DIR PLANK_RELAY_SOURCE_DIR PLANK_RELAY_SODIUM_PREFIX)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required; use the pinned native-client build inputs")
    endif()
endforeach()
file(GLOB models CONFIGURE_DEPENDS "${VISION}/Sources/Models/*.swift")
file(GLOB services CONFIGURE_DEPENDS "${VISION}/Sources/Services/*.swift")
list(FILTER services EXCLUDE REGEX "/PlankAudioOutput.swift$")
add_executable(PlankMac MACOSX_BUNDLE
    ${models} ${services}
    ${VISION}/Bridge/PlankAddress.c
    ${VISION}/Bridge/PlankAudioDecoder.c
    ${VISION}/Bridge/PlankAudioRing.c
    ${VISION}/Bridge/PlankTransportBridge.c
    ${VISION}/Bridge/PlankRawHidFrame.c
    ${VISION}/Bridge/PlankVideoDecoder.c
    Sources/PlankMacApp.swift
    Sources/PlankMacBrowser.swift
    Sources/PlankMacDesktop.swift
    Sources/PlankMacVideoView.swift
    Sources/PlankMacInput.swift
    Sources/PlankMacCursorOverlay.swift
    Sources/PlankMacWindowPresentation.swift
    Sources/PlankMacLocalControls.swift
    Sources/PlankMacInputPolicy.swift
    Sources/PlankMacWacomSession.swift
    Bridge/PlankMacWacom.cpp
    ../app/streaming/input/macrawwacom.cpp
    Sources/PlankMacAudioOutput.swift
    ${VISION}/PrivacyInfo.xcprivacy)
set_source_files_properties(${VISION}/PrivacyInfo.xcprivacy PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set(mac_icon "${CMAKE_CURRENT_BINARY_DIR}/branding/PlankMac.icns")
execute_process(COMMAND xcrun --sdk macosx swift
    "${CMAKE_CURRENT_SOURCE_DIR}/Branding/render-mac-icon.swift"
    "${VISION}/Branding/plank-logo.png" "${mac_icon}" COMMAND_ERROR_IS_FATAL ANY)
set_source_files_properties(${mac_icon} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_sources(PlankMac PRIVATE ${mac_icon})
set(rust_target "aarch64-apple-darwin")
set(rust_build "${CMAKE_CURRENT_BINARY_DIR}/transport/macos-${CMAKE_OSX_DEPLOYMENT_TARGET}")
set(rust_library "${rust_build}/${rust_target}/release/libplank_transport.a")
add_custom_command(OUTPUT ${rust_library}
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${rust_build}"
        "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        cargo build --locked --offline --release --target ${rust_target}
        --manifest-path "${PLANK_TRANSPORT_DIR}/Cargo.toml"
    DEPENDS ${PLANK_TRANSPORT_DIR}/Cargo.toml ${PLANK_TRANSPORT_DIR}/Cargo.lock VERBATIM)
add_custom_target(PlankMacTransport DEPENDS ${rust_library})
add_dependencies(PlankMac PlankMacTransport)
add_library(PlankMacRelayCrypto STATIC
    ${PLANK_RELAY_SOURCE_DIR}/src/protocol.c ${PLANK_RELAY_SOURCE_DIR}/src/session.c
    ${PLANK_RELAY_SOURCE_DIR}/src/noise.c ${PLANK_RELAY_SOURCE_DIR}/src/cpace.c
    ${PLANK_RELAY_SOURCE_DIR}/src/confirm.c ${PLANK_RELAY_SOURCE_DIR}/src/link.c
    ${PLANK_RELAY_SOURCE_DIR}/src/client_link.c ${PLANK_RELAY_SOURCE_DIR}/src/client_pair.c
    ${PLANK_RELAY_SOURCE_DIR}/src/client_enrollment.c)
target_include_directories(PlankMacRelayCrypto PUBLIC ${PLANK_RELAY_SOURCE_DIR}/src ${PLANK_RELAY_SODIUM_PREFIX}/include)
target_link_libraries(PlankMacRelayCrypto PUBLIC ${PLANK_RELAY_SODIUM_PREFIX}/lib/libsodium.a)
target_include_directories(PlankMac PRIVATE ${PLANK_TRANSPORT_DIR}/include
    ${VISION}/Bridge ${CMAKE_CURRENT_SOURCE_DIR}/Bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/streaming/input
    ${CMAKE_CURRENT_SOURCE_DIR}/../moonlight-common-c/moonlight-common-c/src
    ${PLANK_FFMPEG_DIR}/include ${PLANK_OPUS_DIR}/include/opus)
target_compile_definitions(PlankMac PRIVATE PLANK_NATIVE_TRANSPORT=1 PLANK_TABLET_RELAY=1 PLANK_OPUS_AUDIO=1 PLANK_NATIVE_MAC_WACOM=1)
target_compile_options(PlankMac PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-DPLANK_TABLET_RELAY;-DPLANK_NATIVE_MAC_WACOM>")
target_link_libraries(PlankMac PRIVATE ${rust_library} PlankMacRelayCrypto
    ${PLANK_OPUS_DIR}/lib/libopus.a
    ${PLANK_FFMPEG_DIR}/lib/libavcodec.dylib ${PLANK_FFMPEG_DIR}/lib/libavutil.dylib
    ${PLANK_FFMPEG_DIR}/lib/libswscale.dylib ${PLANK_FFMPEG_DIR}/lib/libswresample.dylib
    "-framework AppKit" "-framework Metal" "-framework QuartzCore"
    "-framework Security" "-framework SystemConfiguration" "-framework GameController"
    "-framework AVFAudio" "-framework CoreAudio" "-framework CoreBluetooth"
    "-framework AudioToolbox" "-framework VideoToolbox" "-framework CoreFoundation"
    "-framework IOKit" "-framework CoreMedia" "-framework CoreVideo" -lz -liconv)
set_target_properties(PlankMac PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/Bridge/PlankMac-Bridging-Header.h
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER la.instinctual.PLANK.NativeMac
    XCODE_ATTRIBUTE_PRODUCT_NAME "PLANK Native Pilot"
    XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
    XCODE_ATTRIBUTE_MARKETING_VERSION 0.1.0
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION ${PLANK_APPLE_BUILD_NUMBER}
    XCODE_ATTRIBUTE_PLANK_BUILD_BRANCH ${PLANK_BUILD_BRANCH}
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    XCODE_ATTRIBUTE_SWIFT_STRICT_CONCURRENCY complete
    XCODE_ATTRIBUTE_SWIFT_VERSION 6.0)
