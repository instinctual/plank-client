cmake_minimum_required(VERSION 3.30)
project(PlankIPad LANGUAGES Swift C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "Use an iOS device or simulator SDK")
endif()
set(CMAKE_Swift_LANGUAGE_VERSION 6)
set(PLANK_IPAD_BUILD_NUMBER "33" CACHE STRING "Separate iPad pilot build")
set(VISION "${CMAKE_CURRENT_SOURCE_DIR}/../visionos-native")
foreach(required PLANK_TRANSPORT_DIR PLANK_FFMPEG_DIR PLANK_OPUS_DIR PLANK_RELAY_SOURCE_DIR PLANK_RELAY_SODIUM_PREFIX)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required; use scripts/apple-native/build.py")
    endif()
endforeach()
file(GLOB models CONFIGURE_DEPENDS "${VISION}/Sources/Models/*.swift")
file(GLOB services CONFIGURE_DEPENDS "${VISION}/Sources/Services/*.swift")
list(FILTER services EXCLUDE REGEX "/PlankMouseCapture.swift$")
add_executable(PlankIPad MACOSX_BUNDLE
    ${models} ${services}
    ${VISION}/Bridge/PlankAddress.c
    ${VISION}/Bridge/PlankAudioDecoder.c
    ${VISION}/Bridge/PlankAudioRing.c
    ${VISION}/Bridge/PlankTransportBridge.c
    ${VISION}/Bridge/PlankRawHidFrame.c
    ${VISION}/Bridge/PlankVideoDecoder.c
    ${VISION}/Sources/Views/PlankMetalVideoView.swift
    Sources/PlankIPadApp.swift Sources/PlankIPadInput.swift
    Sources/PlankIPadInputPolicy.swift Sources/PlankIPadVideo.swift
    Sources/PlankIPadKeys.swift Sources/PlankIPadKeyboardViewport.swift ${VISION}/PrivacyInfo.xcprivacy)
target_sources(PlankIPad PRIVATE Sources/PlankIPadDisplayOptions.swift Sources/PlankIPadPencilPolicy.swift
    Sources/PlankIPadPencilRelay.swift Sources/PlankIPadPencilPadSettings.swift
    Sources/PlankIPadCustomControls.swift Sources/PlankIPadCustomControlModel.swift
    Sources/PlankIPadControlContactPolicy.swift Sources/PlankIPadControlAppearance.swift
    Sources/PlankIPadWorkingSurface.swift
    ../apple-native/Shared/PlankControlKeyOwnership.swift ../apple-native/Shared/PlankPencilModifiers.swift
    ../apple-native/Shared/PlankPencilRelayWire.swift
    ../apple-native/Shared/PlankPencilRelayKeys.swift
    ../apple-native/Shared/PlankPencilRelayPeer.swift
    ../apple-native/Shared/PlankPencilCrypto.c)
target_include_directories(PlankIPad PRIVATE ../apple-native/Shared)
target_compile_definitions(PlankIPad PRIVATE PLANK_PENCIL_RELAY=1)
set_source_files_properties(${VISION}/PrivacyInfo.xcprivacy PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
    set(rust_target "aarch64-apple-ios-sim")
else()
    set(rust_target "aarch64-apple-ios")
endif()
set(rust_build "${CMAKE_CURRENT_BINARY_DIR}/transport")
set(rust_library "${rust_build}/${rust_target}/release/libplank_transport.a")
add_custom_command(OUTPUT ${rust_library}
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${rust_build}"
        "IPHONEOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        cargo build --locked --offline --release --target ${rust_target}
        --manifest-path "${PLANK_TRANSPORT_DIR}/Cargo.toml"
    DEPENDS ${PLANK_TRANSPORT_DIR}/Cargo.toml ${PLANK_TRANSPORT_DIR}/Cargo.lock VERBATIM)
add_custom_target(PlankIPadTransport DEPENDS ${rust_library})
add_dependencies(PlankIPad PlankIPadTransport)
add_library(PlankIPadRelayCrypto STATIC
    ${PLANK_RELAY_SOURCE_DIR}/src/protocol.c ${PLANK_RELAY_SOURCE_DIR}/src/session.c
    ${PLANK_RELAY_SOURCE_DIR}/src/noise.c ${PLANK_RELAY_SOURCE_DIR}/src/cpace.c
    ${PLANK_RELAY_SOURCE_DIR}/src/confirm.c ${PLANK_RELAY_SOURCE_DIR}/src/link.c
    ${PLANK_RELAY_SOURCE_DIR}/src/client_link.c ${PLANK_RELAY_SOURCE_DIR}/src/client_pair.c
    ${PLANK_RELAY_SOURCE_DIR}/src/client_enrollment.c)
target_include_directories(PlankIPadRelayCrypto PUBLIC ${PLANK_RELAY_SOURCE_DIR}/src ${PLANK_RELAY_SODIUM_PREFIX}/include)
target_link_libraries(PlankIPadRelayCrypto PUBLIC ${PLANK_RELAY_SODIUM_PREFIX}/lib/libsodium.a)
target_include_directories(PlankIPad PRIVATE ${PLANK_TRANSPORT_DIR}/include
    ${PLANK_FFMPEG_DIR}/include ${PLANK_OPUS_DIR}/include/opus)
target_compile_definitions(PlankIPad PRIVATE PLANK_NATIVE_TRANSPORT=1 PLANK_TABLET_RELAY=1 PLANK_OPUS_AUDIO=1)
target_compile_options(PlankIPad PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-DPLANK_TABLET_RELAY>")
target_link_libraries(PlankIPad PRIVATE ${rust_library} PlankIPadRelayCrypto
    ${PLANK_OPUS_DIR}/lib/libopus.a
    ${PLANK_FFMPEG_DIR}/lib/libavcodec.a ${PLANK_FFMPEG_DIR}/lib/libavutil.a
    ${PLANK_FFMPEG_DIR}/lib/libswscale.a ${PLANK_FFMPEG_DIR}/lib/libswresample.a
    "-framework UIKit" "-framework SwiftUI" "-framework Metal" "-framework QuartzCore"
    "-framework Security" "-framework SystemConfiguration" "-framework GameController"
    "-framework AVFAudio" "-framework CoreAudio" "-framework CoreBluetooth"
    "-framework AudioToolbox" "-framework VideoToolbox" "-framework CoreFoundation"
    "-framework CoreMedia" "-framework CoreVideo" -lz -liconv)
set_target_properties(PlankIPad PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER ${VISION}/Bridge/PlankVision-Bridging-Header.h
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.instinctual.plank.ipadpilot
    XCODE_ATTRIBUTE_PRODUCT_NAME "PLANK iPad Pilot"
    XCODE_ATTRIBUTE_PRODUCT_MODULE_NAME PlankIPad
    XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
    XCODE_ATTRIBUTE_MARKETING_VERSION 0.1.0
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION ${PLANK_IPAD_BUILD_NUMBER}
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
    XCODE_ATTRIBUTE_SWIFT_STRICT_CONCURRENCY complete
    XCODE_ATTRIBUTE_SWIFT_VERSION 6.0
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2")
