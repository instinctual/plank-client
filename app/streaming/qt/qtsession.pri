!linux: error("The Qt-owned session prototype is Ubuntu-only")
QT += core gui qml quick quickcontrols2
CONFIG += c++17
INCLUDEPATH += $$PWD
SOURCES += $$PWD/sessionwindow.cpp $$PWD/sessionshell.cpp $$PWD/cursoritem.cpp
HEADERS += $$PWD/sessionwindow.h $$PWD/sessiongeometry.h $$PWD/sessionshell.h $$PWD/cursoritem.h $$PWD/sessionshortcut.h
RESOURCES += $$PWD/qtsession.qrc
