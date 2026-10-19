QT += core gui testlib
QT -= widgets
TEMPLATE = app
CONFIG += console testcase c++17
DEFINES += PLANK_QT_CLIENT_EXPERIMENT
INCLUDEPATH += ../../app
SOURCES += test_qtsystemproperties.cpp ../../app/backend/systemproperties.cpp
HEADERS += ../../app/backend/systemproperties.h
