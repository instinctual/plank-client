QT += core quick network quickcontrols2 svg
CONFIG += c++17

unix:contains(CONFIG, plank-transport) {
    isEmpty(PLANK_TRANSPORT_DIR) {
        PLANK_TRANSPORT_DIR = $$(PLANK_TRANSPORT_DIR)
    }
    isEmpty(PLANK_TRANSPORT_DIR) {
        PLANK_TRANSPORT_DIR = $$clean_path($$PWD/../../../protocol/plank-transport)
    }
    !exists($$PLANK_TRANSPORT_DIR/Cargo.toml) {
        error("PLANK native transport Cargo.toml is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport.h) {
        error("PLANK native transport header is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_control.h) {
        error("PLANK transport control header is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_input.h) {
        error("PLANK transport input header is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_event.h) {
        error("PLANK transport event header is missing: $$PLANK_TRANSPORT_DIR")
    }

    PLANK_CARGO = $$(CARGO)
    isEmpty(PLANK_CARGO): PLANK_CARGO = cargo
    PLANK_TRANSPORT_CARGO_TARGET_DIR = $$OUT_PWD/plank-transport-cargo
    PLANK_TRANSPORT_LIBRARY = $$PLANK_TRANSPORT_CARGO_TARGET_DIR/release/libplank_transport.a

    plank_transport.target = $$PLANK_TRANSPORT_LIBRARY
    plank_transport.depends = FORCE
    plank_transport.commands = \
        CARGO_TARGET_DIR=$$shell_quote($$PLANK_TRANSPORT_CARGO_TARGET_DIR) \
        $$shell_quote($$PLANK_CARGO) build --locked --offline --release \
        --manifest-path $$shell_quote($$PLANK_TRANSPORT_DIR/Cargo.toml)
    QMAKE_EXTRA_TARGETS += plank_transport
    PRE_TARGETDEPS += $$PLANK_TRANSPORT_LIBRARY
    QMAKE_CLEAN += $$PLANK_TRANSPORT_CARGO_TARGET_DIR

    INCLUDEPATH += $$PLANK_TRANSPORT_DIR/include
    LIBS += $$PLANK_TRANSPORT_LIBRARY -ldl -lpthread -lm
    !macx: LIBS += -lrt
    macx: LIBS += -framework Security -framework SystemConfiguration
    DEFINES += PLANK_TRANSPORT=1
}

TARGET = plank-client
PLANK_PRIVATE_LIB_NAME = plank
# Both desktop platforms share the session scene and lifecycle. Their GPU,
# native-window and audio adapters remain platform-specific.
linux|macx: CONFIG += plank-qt-session
contains(CONFIG, plank-qt-session) {
    !linux:!macx: error("The Qt session frontend supports Linux and macOS")
    include(streaming/qt/qtsession.pri)
    include(streaming/qt/qtvideo.pri)
    linux: include(streaming/qt/qtwaylandguard.pri)
    SOURCES += streaming/qt/sessionruntime.cpp streaming/qt/decodedvideorenderer.cpp streaming/qt/sessioninput.cpp
    HEADERS += streaming/qt/sessioninput.h streaming/qt/remotekeymap.h streaming/qt/linuxkeymap.h streaming/qt/mackeymap.h
    HEADERS += streaming/qt/sessionruntime.h streaming/qt/decodedvideorenderer.h streaming/qt/decodedvideodelivery.h
}
HEADERS += clientidentity.h

include(../globaldefs.pri)

# Precompile QML files to avoid writing qmlcache on portable versions.
# Since this binds the app against the Qt runtime version, we will only
# do this for Windows and Mac (when disable-prebuilts is not defined),
# since they always ship with the matching build of the Qt runtime.
!disable-prebuilts {
    win32|macx {
        CONFIG(release, debug|release) {
            CONFIG += qtquickcompiler
        }
    }
}

TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS
contains(CONFIG, plank-frame-flow-trace): DEFINES += PLANK_FRAME_FLOW_TRACE

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

!linux:!macx: error("Supported desktop Clients are Linux and macOS")
CONFIG += link_pkgconfig ffmpeg
PKGCONFIG += openssl opus libavcodec libavutil libswscale libswresample
linux {
    PKGCONFIG += libpipewire-0.3 wayland-client x11 egl libdrm libva libplacebo
    DEFINES += HAS_X11 HAS_WAYLAND HAVE_EGL HAVE_DRM HAVE_LIBVA HAVE_LIBPLACEBO_VULKAN
    SOURCES += streaming/audio/linuxmicrophone.cpp
    HEADERS += streaming/audio/linuxmicrophone.h streaming/audio/microphonecapturequeue.h streaming/audio/microphonecaptureclock.h
    packagesExist(libinput):packagesExist(libudev) {
        DEFINES += HAVE_LIBINPUT_TABLET
        PKGCONFIG += libinput libudev
    }
}
macx {
    LIBS += -lobjc -framework VideoToolbox -framework AVFoundation -framework CoreVideo -framework CoreGraphics -framework CoreMedia -framework AppKit -framework Metal -framework QuartzCore -framework AudioToolbox -framework CoreAudio
    # Resolve prepared native dylibs in the build and let macdeployqt copy
    # their dependency closure. Packaging removes this development RPATH.
    QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir opus)
}

SOURCES += \
    backend/nvaddress.cpp \
    backend/outputtopology.cpp \
    backend/nvapp.cpp \
    main.cpp \
    backend/computerseeker.cpp \
    backend/nvcomputer.cpp \
    backend/nvhttp.cpp \
    backend/hosttruststore.cpp \
    backend/hosttlsguard.cpp \
    backend/computermanager.cpp \
    backend/relaywakeclient.cpp \
    backend/internetconnectivity.cpp \
    cli/commandlineparser.cpp \
    cli/startstream.cpp \
    settings/plankclientpolicy.cpp \
    settings/streamingpreferences.cpp \
    streaming/session.cpp \
    streaming/audio/microphone.cpp \
    streaming/camera/camera.cpp \
    streaming/avsynccontroller.cpp \
    streaming/plankdisplaymode.cpp \
    streaming/plankpresentation.cpp \
    streaming/audio/audio.cpp \
    gui/computermodel.cpp \
    streaming/bandwidth.cpp \
    path.cpp \
    backend/systemproperties.cpp \
    wm.cpp

macx: HEADERS += macapplication.h

HEADERS += \
    backend/authenticationtakeover.h \
    streaming/video/packedbt709.h \
    backend/nvaddress.h \
    backend/outputtopology.h \
    backend/nvapp.h \
    utils.h \
    backend/computerseeker.h \
    backend/nvcomputer.h \
    backend/planknetwork.h \
    backend/nvhttp.h \
    backend/hosttruststore.h \
    backend/hosttlsguard.h \
    backend/desktopstage.h \
    backend/hostrecovery.h \
    backend/computermanager.h \
    backend/relaywakeclient.h \
    cli/commandlineparser.h \
    cli/startstream.h \
    settings/streamingpreferences.h \
    settings/plankclientpolicy.h \
    backend/internetconnectivity.h \
    streaming/avsynccontroller.h \
    streaming/input/plankpointerlogic.h \
    streaming/session.h \
    streaming/plankdisplaymode.h \
    streaming/plankpresentation.h \
    streaming/planktoolbarlogic.h \
    streaming/planktoolbarstats.h \
    streaming/plankreconnectpolicy.h \
    streaming/audio/renderers/renderer.h \
    gui/computermodel.h \
    streaming/video/decoder.h \
    streaming/bandwidth.h \
    path.h \
    backend/systemproperties.h
HEADERS += backend/clientdisplays.h

linux {
    SOURCES += streaming/audio/renderers/pipewire.cpp
    HEADERS += streaming/audio/renderers/pipewire.h streaming/audio/renderers/pipewirebuffer.h
}

contains(DEFINES, HAVE_LIBINPUT_TABLET) {
    SOURCES += streaming/input/linuxwacom.cpp
    HEADERS += streaming/input/linuxwacom.h
    SOURCES += streaming/input/linuxrawwacom.cpp
    HEADERS += streaming/input/linuxrawwacom.h
}

# Platform-specific renderers and decoders
linux:contains(CONFIG, plank-transport) {
    SOURCES += streaming/camera/linuxnativecamera.cpp
    HEADERS += streaming/camera/linuxnativecamera.h \
        streaming/camera/nativecameraframe.h streaming/camera/uvch264control.h
}

DEFINES += HAVE_FFMPEG
SOURCES += streaming/video/ffmpeg.cpp
HEADERS += streaming/video/ffmpeg.h streaming/video/ffmpeg-renderers/renderer.h

macx {
    message(VideoToolbox renderer selected)

    DEFINES += HAVE_MAC_RAW_WACOM
    SOURCES += streaming/input/macrawwacom.cpp
    HEADERS += streaming/input/macrawwacom.h streaming/input/macrawwacomlogic.h streaming/input/macrawwacomasync.h
    HEADERS += streaming/input/macrawwacomfocus.h
    LIBS += -framework IOKit -framework CoreFoundation -framework ApplicationServices -framework Carbon

    SOURCES += \
        streaming/macquitshortcut.mm \
        streaming/mackeyboardcapture.mm

    HEADERS += \
        streaming/macquitshortcut.h \
        streaming/mackeyboardcapture.h \
        streaming/macdisplaygeometry.h \
        streaming/macdisplaymode.h \
        streaming/macclipboardsync.h \
        streaming/plankclipboard.h
    OBJECTIVE_SOURCES += streaming/macclipboardsync.mm streaming/macdisplays.mm
    HEADERS += streaming/macdisplays.h
    OBJECTIVE_SOURCES += streaming/audio/macmicrophonepermission.mm streaming/audio/coreaudiodevice.mm streaming/audio/renderers/coreaudio.mm streaming/audio/macmicrophone.mm
    HEADERS += streaming/audio/coreaudiodevice.h streaming/audio/renderers/coreaudio.h streaming/audio/macmicrophone.h streaming/audio/coreaudioplaybackstate.h
    OBJECTIVE_SOURCES += backend/macpermissions.mm
    HEADERS += backend/macpermissions.h
}
embedded {
    message(Embedded build)

    DEFINES += EMBEDDED_BUILD
}
glslow {
    message(GL slow build)

    DEFINES += GL_IS_SLOW
}
vkslow {
    message(Vulkan slow build)

    DEFINES += VULKAN_IS_SLOW
}
gpuslow {
    message(GPU slow build)

    DEFINES += GL_IS_SLOW VULKAN_IS_SLOW
}
wayland {
    message(Wayland extensions enabled)

    DEFINES += HAS_WAYLAND
    HEADERS += \
        streaming/plankcallbacktarget.h
}

RESOURCES += \
    resources.qrc \
    qml.qrc

TRANSLATIONS += \
    languages/qml_zh_CN.ts \
    languages/qml_de.ts \
    languages/qml_fr.ts \
    languages/qml_nb_NO.ts \
    languages/qml_ru.ts \
    languages/qml_es.ts \
    languages/qml_ja.ts \
    languages/qml_vi.ts \
    languages/qml_th.ts \
    languages/qml_ko.ts \
    languages/qml_hu.ts \
    languages/qml_nl.ts \
    languages/qml_sv.ts \
    languages/qml_tr.ts \
    languages/qml_uk.ts \
    languages/qml_zh_TW.ts \
    languages/qml_el.ts \
    languages/qml_hi.ts \
    languages/qml_it.ts \
    languages/qml_pt.ts \
    languages/qml_pt_BR.ts \
    languages/qml_pl.ts \
    languages/qml_cs.ts \
    languages/qml_he.ts \
    languages/qml_ckb.ts \
    languages/qml_lt.ts \
    languages/qml_et.ts \
    languages/qml_bg.ts \
    languages/qml_eo.ts \
    languages/qml_ta.ts

# Additional import path used to resolve QML modules in Qt Creator's code model
QML_IMPORT_PATH =

# Additional import path used to resolve QML modules just for Qt Quick Designer
QML_DESIGNER_IMPORT_PATH =

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../moonlight-common-c/release/ -lmoonlight-common-c
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../moonlight-common-c/debug/ -lmoonlight-common-c
else:unix: LIBS += -L$$OUT_PWD/../moonlight-common-c/ -lmoonlight-common-c

INCLUDEPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src
DEPENDPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../qmdnsengine/release/ -lqmdnsengine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../qmdnsengine/debug/ -lqmdnsengine
else:unix: LIBS += -L$$OUT_PWD/../qmdnsengine/ -lqmdnsengine

INCLUDEPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine
DEPENDPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../h264bitstream/release/ -lh264bitstream
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../h264bitstream/debug/ -lh264bitstream
else:unix: LIBS += -L$$OUT_PWD/../h264bitstream/ -lh264bitstream

INCLUDEPATH += $$PWD/../h264bitstream
DEPENDPATH += $$PWD/../h264bitstream

!winrt {
    win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../AntiHooking/release/ -lAntiHooking
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../AntiHooking/debug/ -lAntiHooking

    INCLUDEPATH += $$PWD/../AntiHooking
    DEPENDPATH += $$PWD/../AntiHooking
}

unix:!macx: {
    # The packaged Linux executable lives in /usr/bin while its qualified
    # FFmpeg runtime remains private to PLANK under /usr/lib/plank. Resolve
    # those libraries relative to the executable instead of relying on a
    # launcher-provided LD_LIBRARY_PATH.
    QMAKE_LFLAGS += "-Wl,-rpath,'\$$ORIGIN/../lib/$$PLANK_PRIVATE_LIB_NAME'"

    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }
    isEmpty(DATADIR) {
        DATADIR = share
    }

    target.path = $$PREFIX/$$BINDIR/

    desktop.files = deploy/linux/la.instinctual.Plank.Client.desktop
    desktop.path = $$PREFIX/$$DATADIR/applications/

    icons.files = res/plank-logo.png
    icons.path = $$PREFIX/$$DATADIR/icons/hicolor/512x512/apps/

    appstream.files = deploy/linux/la.instinctual.Plank.Client.appdata.xml
    appstream.path = $$PREFIX/$$DATADIR/metainfo/

    INSTALLS += target desktop icons appstream
}
win32 {
    RC_ICONS = moonlight.ico
    QMAKE_TARGET_COMPANY = Instinctual
    QMAKE_TARGET_DESCRIPTION = PLANK Client
    QMAKE_TARGET_PRODUCT = PLANK

    CONFIG -= embed_manifest_exe
    QMAKE_LFLAGS += /MANIFEST:embed /MANIFESTINPUT:$${PWD}/plank-client.exe.manifest
}
macx {
    # One SDK27-built arm64 application for macOS15 and newer, not separate
    # reduced-capability and modern editions. New APIs need availability guards.
    isEmpty(PLANK_MAC_CLIENT_MIN_MACOS): PLANK_MAC_CLIENT_MIN_MACOS = 15.0
    !equals(PLANK_MAC_CLIENT_MIN_MACOS, 15.0) {
        error("PLANK Client deployment target must be 15.0 (build with SDK27 or newer)")
    }
    QMAKE_MACOSX_DEPLOYMENT_TARGET = $$PLANK_MAC_CLIENT_MIN_MACOS
    QMAKE_APPLE_DEVICE_ARCHS = arm64
    QMAKE_CFLAGS += -Werror=unguarded-availability-new
    QMAKE_CXXFLAGS += -Werror=unguarded-availability-new
    QMAKE_INFO_PLIST = $$PWD/Info.plist

    APP_BUNDLE_RESOURCES.files = moonlight.icns
    APP_BUNDLE_RESOURCES.path = Contents/Resources

    QMAKE_BUNDLE_DATA += APP_BUNDLE_RESOURCES

    !disable-prebuilts {
        APP_BUNDLE_FRAMEWORKS.files = $$files(../libs/mac/Frameworks/*.framework, true) $$files(../libs/mac/lib/*.dylib, true)
        APP_BUNDLE_FRAMEWORKS.path = Contents/Frameworks

        QMAKE_BUNDLE_DATA += APP_BUNDLE_FRAMEWORKS

        QMAKE_RPATHDIR += @executable_path/../Frameworks
    }
}

isEmpty(PLANK_VERSION) {
    PLANK_VERSION = development
}
VERSION = "$$section(PLANK_VERSION, -, 0, 0)"
DEFINES += PLANK_VERSION_STR=\\\"$$PLANK_VERSION\\\"
