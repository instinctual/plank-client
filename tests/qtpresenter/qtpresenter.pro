QT += core gui quick testlib
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app
include(../../app/streaming/qt/qtvideo.pri)
PKGCONFIG += sdl3 sdl3-ttf
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_qtpresenter.cpp ../../app/streaming/qt/decodedvideorenderer.cpp ../../app/streaming/avsynccontroller.cpp
HEADERS += ../../app/streaming/qt/decodedvideorenderer.h
QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir libavutil) $$system(pkg-config --variable=libdir sdl3)
QMAKE_LFLAGS += -Wl,--disable-new-dtags
