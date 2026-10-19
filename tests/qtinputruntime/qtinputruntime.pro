QT += core gui testlib
QT -= widgets
TEMPLATE = app
CONFIG += console testcase c++17
INCLUDEPATH += ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_qtinputruntime.cpp ../../app/streaming/qt/sessioninput.cpp
HEADERS += ../../app/streaming/qt/sessioninput.h
LIBS += -pthread
