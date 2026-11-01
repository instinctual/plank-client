!macx: error("This test requires macOS and Metal")
QT += core gui quick
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = macqtmetal
OBJECTIVE_SOURCES += test_macqtmetal.mm
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
PKGCONFIG += libavcodec libavutil
SOURCES += ../../app/streaming/qt/videoframe.cpp \
    ../../app/streaming/qt/decodedvideorenderer.cpp ../../app/streaming/avsynccontroller.cpp
QMAKE_CXXFLAGS += -fobjc-arc
LIBS += -framework Metal -framework QuartzCore
