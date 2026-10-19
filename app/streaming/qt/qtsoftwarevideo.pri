!linux: error("The Qt software video experiment is Ubuntu-only")
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libavcodec libavutil libplacebo vulkan libdrm
INCLUDEPATH += $$PWD
SOURCES += $$PWD/softwarevideoconverter.cpp \
    $$PWD/../video/ffmpeg-renderers/plvkframepool.cpp \
    $$PWD/../video/ffmpeg-renderers/plvk_c.c
HEADERS += $$PWD/softwarevideoconverter.h $$PWD/../video/ffmpeg-renderers/plvkframepool.h
