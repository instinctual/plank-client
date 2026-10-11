!linux: error("The Qt-owned session prototype is Ubuntu-only")
QT += core gui qml quick quickcontrols2
CONFIG += c++17
INCLUDEPATH += $$PWD
SOURCES += $$PWD/sessionwindow.cpp
HEADERS += $$PWD/sessionwindow.h $$PWD/sessiongeometry.h
RESOURCES += $$PWD/qtsession.qrc
