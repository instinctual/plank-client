cmake_minimum_required(VERSION 3.30)

project(PlankVision LANGUAGES Swift C)

set(CMAKE_Swift_LANGUAGE_VERSION 6)
set(PLANK_VISION_BUILD_NUMBER "1" CACHE STRING "Monotonically increasing TestFlight build number")
set(PLANK_VISION_VERSION "0.1.0" CACHE STRING "TestFlight marketing version")
set(PLANK_VISION_TEAM_ID "" CACHE STRING "Signing team; empty for public unsigned builds")
set(PLANK_VISION_BUNDLE_ID "la.instinctual.PLANK.Vision" CACHE STRING "Team-owned application identifier")
set(PLANK_BUILD_BRANCH "local" CACHE STRING "Source branch shown in the pilot")
if(NOT PLANK_VISION_BUILD_NUMBER MATCHES "^[1-9][0-9]*([.][0-9]+)?([.][0-9]+)?$")
    message(FATAL_ERROR "PLANK_VISION_BUILD_NUMBER must be a numeric Apple build version")
endif()
if(NOT PLANK_VISION_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
    message(FATAL_ERROR "PLANK_VISION_VERSION must have three numeric components")
endif()

add_executable(PlankVision MACOSX_BUNDLE
    Bridge/PlankAddress.c
    Bridge/PlankAddress.h
    Bridge/PlankAudioDecoder.c
    Bridge/PlankAudioDecoder.h
    Bridge/PlankAudioRing.c
    Bridge/PlankAudioRing.h
    Bridge/PlankTransportBridge.c
    Bridge/PlankTransportBridge.h
    Bridge/PlankRawHidFrame.c
    Bridge/PlankRawHidFrame.h
    Bridge/PlankVideoDecoder.c
    Bridge/PlankVideoDecoder.h
    Sources/PlankVisionApp.swift
    Sources/Models/HostBookmark.swift
    Sources/Models/PlankAudioFormat.swift
    Sources/Models/PlankAudioLevel.swift
    Sources/Models/PlankStreamRequest.swift
    Sources/Services/HostDiscovery.swift
    Sources/Services/HostStore.swift
    Sources/Services/PlankAudioOutput.swift
    Sources/Services/PlankAudioReceiver.swift
    Sources/Services/PlankHTTPClient.swift
    Sources/Services/PlankCoreClient.swift
    Sources/Services/PlankSessionStartupDeadline.swift
    Sources/Services/PlankHardwareVideoDecoder.swift
    Sources/Services/PlankVideoDecoderRecovery.swift
    Sources/Services/PlankControlReceiver.swift
    Sources/Services/PlankSessionFocus.swift
    Sources/Services/PlankMouseCapture.swift
    Sources/Services/PlankLiveBitrate.swift
    Sources/Services/PlankTimingCapture.swift
    Sources/Services/PlankSessionEngine.swift
    Sources/Services/PlankInputQueue.swift
    Sources/Views/AddHostView.swift
    Sources/Views/HostBrowserView.swift
    Sources/Views/HostDetailView.swift
    Sources/Views/PlankSessionControls.swift
    Sources/Views/PlankMetalVideoView.swift
    Sources/Views/RemoteDesktopView.swift
    Sources/Views/SettingsView.swift
    PrivacyInfo.xcprivacy
)
set_source_files_properties(PrivacyInfo.xcprivacy PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

# visionOS requires a layered app icon. Render the existing PLANK artwork into
# separate opaque and transparent layers so the archive is self-contained.
set(PLANK_VISION_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/generated/Assets.xcassets")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets/" DESTINATION "${PLANK_VISION_ASSETS}")
execute_process(COMMAND xcrun --sdk macosx swift
    "${CMAKE_CURRENT_SOURCE_DIR}/Branding/render-vision-icon.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/Branding/plank-logo.png"
    "${PLANK_VISION_ASSETS}"
    COMMAND_ERROR_IS_FATAL ANY)
set_source_files_properties("${PLANK_VISION_ASSETS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(PlankVision PRIVATE "${PLANK_VISION_ASSETS}")

set(PLANK_TRANSPORT_DIR "" CACHE PATH "Path to the plank-transport Cargo project")
if(PLANK_TRANSPORT_DIR)
    if(NOT EXISTS "${PLANK_TRANSPORT_DIR}/Cargo.toml")
        message(FATAL_ERROR "PLANK_TRANSPORT_DIR does not contain Cargo.toml")
    endif()

    if(CMAKE_OSX_SYSROOT MATCHES "xrsimulator")
        set(PLANK_RUST_TARGET "aarch64-apple-visionos-sim")
    else()
        set(PLANK_RUST_TARGET "aarch64-apple-visionos")
    endif()

    set(PLANK_TRANSPORT_CARGO_TARGET "${CMAKE_BINARY_DIR}/plank-transport-cargo")
    set(PLANK_TRANSPORT_LIBRARY
        "${PLANK_TRANSPORT_CARGO_TARGET}/${PLANK_RUST_TARGET}/release/libplank_transport.a")

    add_custom_command(
        OUTPUT "${PLANK_TRANSPORT_LIBRARY}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${PLANK_TRANSPORT_CARGO_TARGET}"
            "XROS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            cargo build --locked --release
                --target "${PLANK_RUST_TARGET}"
                --manifest-path "${PLANK_TRANSPORT_DIR}/Cargo.toml"
        DEPENDS
            "${PLANK_TRANSPORT_DIR}/Cargo.toml"
            "${PLANK_TRANSPORT_DIR}/Cargo.lock"
        VERBATIM
    )
    add_custom_target(PlankTransportRust DEPENDS "${PLANK_TRANSPORT_LIBRARY}")
    add_dependencies(PlankVision PlankTransportRust)
    target_include_directories(PlankVision PRIVATE "${PLANK_TRANSPORT_DIR}/include")
    target_link_libraries(PlankVision PRIVATE
        "${PLANK_TRANSPORT_LIBRARY}"
        "-framework Security"
        "-framework SystemConfiguration"
        "-framework GameController"
    )
    target_compile_definitions(PlankVision PRIVATE PLANK_NATIVE_TRANSPORT=1)
endif()

# The tablet Relay protocol is shared with the dedicated Linux Relay. Supply
# its source and a visionOS libsodium build until a distributable artifact is pinned.
set(PLANK_RELAY_SOURCE_DIR "" CACHE PATH "Path to plank-tablet-relay source")
set(PLANK_RELAY_SODIUM_PREFIX "" CACHE PATH "Path to visionOS libsodium prefix")
if(PLANK_RELAY_SOURCE_DIR)
    if(NOT EXISTS "${PLANK_RELAY_SOURCE_DIR}/src/link.c" OR
       NOT EXISTS "${PLANK_RELAY_SODIUM_PREFIX}/lib/libsodium.a" OR
       NOT EXISTS "${PLANK_RELAY_SODIUM_PREFIX}/include/sodium.h")
        message(FATAL_ERROR "Tablet Relay source and visionOS libsodium are required together")
    endif()
    add_library(PlankRelayCrypto STATIC
        "${PLANK_RELAY_SOURCE_DIR}/src/protocol.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/session.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/noise.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/cpace.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/confirm.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/link.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/client_link.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/client_pair.c"
        "${PLANK_RELAY_SOURCE_DIR}/src/client_enrollment.c"
    )
    target_include_directories(PlankRelayCrypto PUBLIC
        "${PLANK_RELAY_SOURCE_DIR}/src"
        "${PLANK_RELAY_SODIUM_PREFIX}/include")
    target_link_libraries(PlankRelayCrypto PUBLIC
        "${PLANK_RELAY_SODIUM_PREFIX}/lib/libsodium.a")
    target_link_libraries(PlankVision PRIVATE PlankRelayCrypto)
    target_compile_definitions(PlankVision PRIVATE PLANK_TABLET_RELAY=1)
    target_compile_options(PlankVision PRIVATE
        "$<$<COMPILE_LANGUAGE:Swift>:-DPLANK_TABLET_RELAY>")
    target_sources(PlankVision PRIVATE
        Sources/Models/PlankDrawingHandoff.swift
        Sources/Models/DrawingRegistrationRequest.swift
        Sources/Models/DrawingRegistrationReceipt.swift
        Sources/Services/PlankRelayEnrollment.swift
        Sources/Models/PlankRelayRegistry.swift
        Sources/Services/PlankDrawingIdentityStore.swift
        Sources/Services/PlankRelayPairing.swift
        Sources/Services/PlankRelayApprovalRoutes.swift
        Sources/Services/PlankRelayConnectionTiming.swift
        Sources/Services/PlankRelayWriteQueue.swift
        Sources/Services/PlankRelayByteTransport.swift
        Sources/Services/PlankRelayBluetoothTransport.swift
        Sources/Services/PlankDrawingL2CAPStream.swift
        Sources/Services/PlankRelayLiveLink.swift
        Sources/Services/PlankWacomPreflight.swift
        Sources/Services/PlankTabletInputPolicy.swift
        Sources/Views/PlankRelayRegistrationSheet.swift)
endif()

# Host audio is decoded with xiph libopus, matching the desktop Client's
# multistream decoder and packet-loss concealment. Build the pinned,
# checksum-verified release with scripts/build-visionos-opus.sh.
set(PLANK_OPUS_DIR "" CACHE PATH "Path to the visionOS libopus installation")
if(PLANK_OPUS_DIR)
    if(NOT EXISTS "${PLANK_OPUS_DIR}/lib/libopus.a" OR
       NOT EXISTS "${PLANK_OPUS_DIR}/include/opus/opus_multistream.h")
        message(FATAL_ERROR "PLANK_OPUS_DIR does not contain libopus")
    endif()
    target_include_directories(PlankVision PRIVATE "${PLANK_OPUS_DIR}/include/opus")
    target_link_libraries(PlankVision PRIVATE "${PLANK_OPUS_DIR}/lib/libopus.a")
    target_compile_definitions(PlankVision PRIVATE PLANK_OPUS_AUDIO=1)
endif()
target_link_libraries(PlankVision PRIVATE "-framework AVFAudio" "-framework CoreBluetooth")

set(PLANK_FFMPEG_DIR "" CACHE PATH "Path to the visionOS FFmpeg installation")
if(PLANK_FFMPEG_DIR)
    target_include_directories(PlankVision PRIVATE "${PLANK_FFMPEG_DIR}/include")
    target_link_libraries(PlankVision PRIVATE
        "${PLANK_FFMPEG_DIR}/lib/libavcodec.a"
        "${PLANK_FFMPEG_DIR}/lib/libswscale.a"
        "${PLANK_FFMPEG_DIR}/lib/libswresample.a"
        "${PLANK_FFMPEG_DIR}/lib/libavutil.a"
        "-framework AudioToolbox"
        "-framework VideoToolbox"
        "-framework CoreFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-lz"
        "-liconv"
    )
endif()

set_target_properties(PlankVision PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/PlankVision.entitlements"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/PlankVision-Bridging-Header.h"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${PLANK_VISION_TEAM_ID}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${PLANK_VISION_BUNDLE_ID}"
    XCODE_ATTRIBUTE_PLANK_BUILD_BRANCH "${PLANK_BUILD_BRANCH}"
    XCODE_ATTRIBUTE_PRODUCT_NAME "PLANK"
    XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE "NO"
    XCODE_ATTRIBUTE_INFOPLIST_KEY_UIApplicationSceneManifest_Generation "YES"
    XCODE_ATTRIBUTE_MARKETING_VERSION "${PLANK_VISION_VERSION}"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "${PLANK_VISION_BUILD_NUMBER}"
    XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
    XCODE_ATTRIBUTE_INSTALL_PATH "/Applications"
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym"
    XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS "YES"
    XCODE_ATTRIBUTE_SWIFT_STRICT_CONCURRENCY "complete"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_GENERATE_SCHEME TRUE
)
