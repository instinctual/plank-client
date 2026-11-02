QT += core gui network qml testlib
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += openssl
TEMPLATE = app
TARGET = bookmarks
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src \
    ../../qmdnsengine ../../qmdnsengine/qmdnsengine/src/include
SOURCES += test_bookmarks.cpp \
    ../../app/backend/computermanager.cpp \
    ../../app/backend/nvcomputer.cpp \
    ../../app/backend/nvhttp.cpp \
    ../../app/backend/nvaddress.cpp \
    ../../app/backend/nvapp.cpp \
    ../../app/backend/outputtopology.cpp \
    ../../app/backend/hosttruststore.cpp \
    ../../app/backend/hosttlsguard.cpp \
    ../../app/settings/plankclientpolicy.cpp
HEADERS += ../../app/backend/computermanager.h ../../app/backend/pollingthread.h \
    ../../app/backend/nvhttp.h
# Link the real discovery implementation; the fixture never starts network
# polling, authenticates or opens a session/device.
SOURCES += $$files($$PWD/../../qmdnsengine/qmdnsengine/src/src/*.cpp)
HEADERS += $$files($$PWD/../../qmdnsengine/qmdnsengine/src/include/qmdnsengine/*.h) \
    $$files($$PWD/../../qmdnsengine/qmdnsengine/src/src/*_p.h)
macx {
    OBJECTIVE_SOURCES += ../../app/streaming/macdisplays.mm
    LIBS += -framework AppKit -framework ApplicationServices
}
