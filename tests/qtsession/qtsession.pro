QT += testlib
CONFIG += console testcase c++17 link_pkgconfig
PKGCONFIG += fontconfig
TEMPLATE = app
include(../../app/streaming/qt/qtsession.pri)
SOURCES += test_qtsession.cpp
