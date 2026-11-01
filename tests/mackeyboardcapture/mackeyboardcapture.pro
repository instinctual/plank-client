QT += core testlib
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
LIBS += -framework AppKit -framework ApplicationServices -framework Carbon
SOURCES += test_mackeyboardcapture.mm
HEADERS += ../../app/streaming/mackeyboardcapture.h
