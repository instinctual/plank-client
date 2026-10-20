QT += core testlib
QT -= gui
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app
PKGCONFIG += libpipewire-0.3 libavutil libswresample sdl3
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_pipewireaudio.cpp ../../app/streaming/audio/renderers/pipewire.cpp ../../app/streaming/avsynccontroller.cpp
HEADERS += ../../app/streaming/audio/renderers/pipewire.h ../../app/streaming/audio/renderers/pipewirebuffer.h
QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir libavutil) $$system(pkg-config --variable=libdir libswresample) $$system(pkg-config --variable=libdir sdl3)
QMAKE_LFLAGS += -Wl,--disable-new-dtags
