QT += core gui quick testlib
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app
include(../../app/streaming/qt/qtvideo.pri)
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_qtpresenter.cpp ../../app/streaming/qt/decodedvideorenderer.cpp ../../app/streaming/avsynccontroller.cpp
HEADERS += ../../app/streaming/qt/decodedvideorenderer.h
QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir libavutil)
QMAKE_LFLAGS += -Wl,--disable-new-dtags
