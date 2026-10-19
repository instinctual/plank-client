QT += core testlib
QT -= gui
CONFIG += console testcase c++17
TEMPLATE = app
PRE_TARGETDEPS += $$PWD/qtsoftwarevideo.pro
include(../../app/streaming/qt/qtsoftwarevideo.pri)
QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir libavutil)
QMAKE_LFLAGS += -Wl,--disable-new-dtags
SOURCES += test_qtsoftwarevideo.cpp
