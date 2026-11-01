QT += core testlib
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
LIBS += -framework AppKit
SOURCES += test_macquitshortcut.mm ../../app/streaming/macquitshortcut.mm
HEADERS += ../../app/streaming/macquitshortcut.h
