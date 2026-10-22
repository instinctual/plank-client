cmake_minimum_required(VERSION 3.30)
project(PLANKInputProbe LANGUAGES Swift)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "This standalone probe requires an iOS device or simulator SDK")
endif()
add_executable(PLANKInputProbe MACOSX_BUNDLE InputProbe.swift ContactLedger.swift)
target_link_libraries(PLANKInputProbe PRIVATE "-framework UIKit" "-framework Foundation")
set_target_properties(PLANKInputProbe PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.instinctual.plank.inputprobe"
  XCODE_ATTRIBUTE_PRODUCT_MODULE_NAME "PLANKInputProbe"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2"
  XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
  XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
)
