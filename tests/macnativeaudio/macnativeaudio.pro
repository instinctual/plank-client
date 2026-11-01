QT += core testlib
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
PKGCONFIG += libavutil libswresample opus
LIBS += -framework AudioToolbox -framework CoreAudio
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_macnativeaudio.cpp ../../app/streaming/avsynccontroller.cpp
OBJECTIVE_SOURCES += ../../app/streaming/audio/renderers/coreaudio.mm \
    ../../app/streaming/audio/coreaudiodevice.mm ../../app/streaming/audio/macmicrophone.mm
