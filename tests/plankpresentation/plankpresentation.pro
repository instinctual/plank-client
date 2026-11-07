QT += core testlib
CONFIG += testcase c++17
TEMPLATE = app

SOURCES += \
    test_plankpresentation.cpp \
    ../../app/streaming/plankpresentation.cpp

HEADERS += ../../app/streaming/plankpresentation.h

INCLUDEPATH += ../../app

CONFIG += link_pkgconfig
