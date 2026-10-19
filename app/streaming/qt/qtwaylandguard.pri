QT += waylandclient gui-private
CONFIG += wayland-scanner link_pkgconfig
PKGCONFIG += wayland-client
PLANK_WAYLAND_PROTOCOLS = $$system(pkg-config --variable=pkgdatadir wayland-protocols)
WAYLANDCLIENTSOURCES += \
    $$PLANK_WAYLAND_PROTOCOLS/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml \
    $$PLANK_WAYLAND_PROTOCOLS/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
SOURCES += $$PWD/waylandsessionguard.cpp
HEADERS += $$PWD/waylandsessionguard.h
