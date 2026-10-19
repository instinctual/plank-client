!linux: error("Qt GPU interop is Ubuntu-only")
QT += gui opengl
include(qtsoftwarevideo.pri)
SOURCES += $$PWD/vulkanglimage.cpp $$PWD/softwarevideodevice.cpp
HEADERS += $$PWD/vulkanglimage.h $$PWD/softwarevideodevice.h
