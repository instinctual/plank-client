!linux: error("The Qt video experiment is Ubuntu-only")
QT += opengl quick
include(qtgpuinterop.pri)
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libavutil libva libdrm egl
SOURCES += $$PWD/vaapivideoframe.cpp $$PWD/packedvideoblitter.cpp $$PWD/eglvideonode.cpp $$PWD/videomailbox.cpp $$PWD/videoframe.cpp
HEADERS += $$PWD/vaapivideoframe.h $$PWD/packedvideoblitter.h $$PWD/eglvideonode.h $$PWD/videomailbox.h $$PWD/videoframe.h $$PWD/../video/packedvideo.h
SOURCES += $$PWD/videopresenter.cpp
HEADERS += $$PWD/videopresenter.h $$PWD/decodedvideodelivery.h
SOURCES += $$PWD/egltextureimport.cpp
HEADERS += $$PWD/egltextureimport.h
