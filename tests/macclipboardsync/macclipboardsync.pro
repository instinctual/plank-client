QT += core gui testlib
DEFINES += PLANK_CLIPBOARD_TEST_PASTEBOARD
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app

LIBS += -framework AppKit

INCLUDEPATH += \
    ../../../../protocol/plank-transport/include \
    ../../app/streaming \
    ../../moonlight-common-c/moonlight-common-c/src

SOURCES += \
    test_macclipboardsync.mm \
    ../../app/streaming/macclipboardsync.mm

HEADERS += \
    ../../app/streaming/macclipboardsync.h \
    ../../app/streaming/plankclipboard.h
