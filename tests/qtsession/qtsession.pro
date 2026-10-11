QT += testlib
CONFIG += console testcase c++17
TEMPLATE = app
include(../../app/streaming/qt/qtsession.pri)
SOURCES += test_qtsession.cpp
