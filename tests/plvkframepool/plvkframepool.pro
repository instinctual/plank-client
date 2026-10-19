QT += core testlib
QT -= gui
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app
PKGCONFIG += libavcodec libavutil libplacebo vulkan
QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir libavutil)
INCLUDEPATH += ../../app/streaming/video/ffmpeg-renderers ../../app/streaming/video
SOURCES += test_plvkframepool.cpp \
    ../../app/streaming/video/ffmpeg-renderers/plvkframepool.cpp \
    ../../app/streaming/video/ffmpeg-renderers/plvk_c.c
QMAKE_LFLAGS += -Wl,--wrap=pl_buf_create -Wl,--wrap=pl_buf_destroy \
    -Wl,--wrap=pl_upload_plane -Wl,--wrap=pl_gpu_finish
