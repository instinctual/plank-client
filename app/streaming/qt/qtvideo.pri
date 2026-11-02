!linux:!macx: error("The Qt video frontend supports Linux and macOS")
CONFIG += c++17 link_pkgconfig
QT += quick
INCLUDEPATH += $$PWD
PKGCONFIG += libavutil
SOURCES += $$PWD/videoitem.cpp $$PWD/videomailbox.cpp $$PWD/videoframe.cpp $$PWD/videopresenter.cpp
HEADERS += $$PWD/videoitem.h $$PWD/videomailbox.h $$PWD/videoframe.h $$PWD/videopresenter.h $$PWD/decodedvideodelivery.h
linux {
QT += opengl quick
include(qtgpuinterop.pri)
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libavutil libva libdrm egl
SOURCES += $$PWD/vaapivideoframe.cpp $$PWD/packedvideoblitter.cpp $$PWD/eglvideonode.cpp
HEADERS += $$PWD/vaapivideoframe.h $$PWD/packedvideoblitter.h $$PWD/packedvideoshaders.h $$PWD/eglvideonode.h $$PWD/../video/packedvideo.h
SOURCES += $$PWD/egltextureimport.cpp
HEADERS += $$PWD/egltextureimport.h

}
macx {
    OBJECTIVE_SOURCES += $$PWD/metalvideonode.mm
    HEADERS += $$PWD/metalvideonode.h
    LIBS += -framework Metal -framework CoreVideo
    RESOURCES += $$PWD/metalvideo.qrc
}
