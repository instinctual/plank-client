QT += core testlib
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app
PKGCONFIG += libavutil libva libdrm
QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir libavutil)
INCLUDEPATH += ../../app/streaming/qt
SOURCES += test_qtvideo.cpp ../../app/streaming/qt/vaapivideoframe.cpp
QMAKE_LFLAGS += -Wl,--wrap=vaSyncSurface2 -Wl,--wrap=vaExportSurfaceHandle
