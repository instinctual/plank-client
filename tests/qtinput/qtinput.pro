QT += core gui testlib
QT -= widgets
TEMPLATE = app
TARGET = qtinput
CONFIG += console testcase c++17
INCLUDEPATH += ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_qtinput.cpp
HEADERS += ../../app/streaming/input/remotecursor.h ../../app/streaming/qt/linuxkeymap.h
