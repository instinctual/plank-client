!linux: error("The Qt video experiment is Ubuntu-only")
QT += opengl quick
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libavutil libva libdrm egl
SOURCES += $$PWD/vaapivideoframe.cpp $$PWD/packedvideoblitter.cpp $$PWD/eglvideonode.cpp $$PWD/videomailbox.cpp
HEADERS += $$PWD/vaapivideoframe.h $$PWD/packedvideoblitter.h $$PWD/eglvideonode.h $$PWD/videomailbox.h $$PWD/../video/packedvideo.h
