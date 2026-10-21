QT += core network testlib
QT -= gui
CONFIG += console testcase c++17
TEMPLATE = app
TARGET = clientidentity
contains(CONFIG, plank-qt-session): DEFINES += PLANK_QT_SESSION
INCLUDEPATH += ../../app
SOURCES += test_clientidentity.cpp ../../app/path.cpp \
    ../../app/settings/plankclientpolicy.cpp ../../app/backend/hosttruststore.cpp
