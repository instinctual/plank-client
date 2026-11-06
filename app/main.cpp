#include <QtGlobal>
#include <QDebug>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QEvent>
#ifdef Q_OS_LINUX
#include "streaming/qt/sessionshell.h"
#endif
#include <QStyleHints>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QQuickStyle>
#include <QMutex>
#include <QtDebug>
#include <QNetworkProxyFactory>
#include <QNetworkInformation>
#include <QPalette>
#include <QFont>
#include <QCursor>
#include <QDir>
#include <QDateTime>
#include <QThreadPool>
#include <QRegularExpression>
#include <QPointer>
#include <QTimer>
#include "clientidentity.h"
#include "backend/internetconnectivity.h"

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <system_error>
#include <thread>

#ifdef Q_OS_MACOS
#include "macapplication.h"
#include "streaming/mackeyboardcapture.h"
#include "streaming/input/macrawwacom.h"
#include "streaming/audio/macmicrophonepermission.h"
#include "backend/macpermissions.h"
#endif

#ifdef HAVE_FFMPEG
#include "streaming/video/ffmpeg.h"
#endif

#if defined(Q_OS_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_6.h>
#endif

#include "cli/startstream.h"
#include "cli/commandlineparser.h"
#include "path.h"
#include "utils.h"
#include "gui/computermodel.h"
#include "backend/computermanager.h"
#include <QSslSocket>
#include "backend/systemproperties.h"
#include "streaming/session.h"
#include "settings/streamingpreferences.h"

#ifdef PLANK_TRANSPORT
#include <plank_transport.h>
#endif

#if defined(Q_OS_WIN32)
#define IS_UNSPECIFIED_HANDLE(x) ((x) == INVALID_HANDLE_VALUE || (x) == NULL)

// Log to file or console dynamically for Windows builds
#define LOG_TO_FILE
#elif defined(Q_OS_LINUX)
// Retain a private per-user log without duplicating routine output to journald
#define LOG_TO_FILE
#elif !defined(QT_DEBUG) && defined(Q_OS_DARWIN)
// Log to file for release Mac builds
#define LOG_TO_FILE
#else
// Log to console for debug Mac builds
#endif

static QTextStream s_LoggerStream(stderr);
static QMutex s_SyncLoggerMutex;
#ifdef LOG_TO_FILE
// Max log file size of 10 MB
#define MAX_LOG_SIZE_BYTES (10 * 1024 * 1024)
static int s_LogBytesWritten = 0;
static bool s_LogLimitReached = false;
static QFile* s_LoggerFile;
static QTextStream s_LoggerFileStream;
#endif

#ifdef HAVE_DRM_MASTER_HOOKS
extern "C" bool g_DisableDrmHooks;
#endif

static QString currentLogTimestamp()
{
    const QDateTime localTime = QDateTime::currentDateTime();
    return localTime.toOffsetFromUtc(localTime.offsetFromUtc()).toString(Qt::ISODateWithMs);
}

void logToLoggerStream(QString& message)
{
    QMutexLocker locker(&s_SyncLoggerMutex);

#if defined(QT_DEBUG) && defined(Q_OS_WIN32)
    // Output log messages to a debugger if attached
    if (IsDebuggerPresent()) {
        thread_local QString lineBuffer;
        lineBuffer += message;
        if (message.endsWith('\n')) {
            OutputDebugStringW(lineBuffer.toStdWString().c_str());
            lineBuffer.clear();
        }
    }
#endif

    // Strip session encryption keys and IVs from the logs

#ifdef LOG_TO_FILE
    if (s_LoggerFileStream.device() != nullptr && !s_LogLimitReached) {
        const int messageBytes = message.toUtf8().size();
        if (s_LogBytesWritten + messageBytes > MAX_LOG_SIZE_BYTES) {
            const QString limitMessage = "Log size limit reached!\n";
            if (s_LogBytesWritten + limitMessage.toUtf8().size() <= MAX_LOG_SIZE_BYTES) {
                s_LoggerFileStream << limitMessage;
                s_LoggerFileStream.flush();
            }
            s_LogLimitReached = true;
        }
        else {
            s_LogBytesWritten += messageBytes;
            s_LoggerFileStream << message;
            s_LoggerFileStream.flush();
        }
    }
#endif

#if !defined(LOG_TO_FILE)
    s_LoggerStream << message;
    s_LoggerStream.flush();
#elif defined(LOG_TO_FILE)
    // Preserve the console fallback if the file could not be opened.
    if (s_LoggerFileStream.device() == nullptr) {
        s_LoggerStream << message;
        s_LoggerStream.flush();
    }
#endif
}

void qtLogToDiskHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    QString typeTxt;

    switch (type) {
    case QtDebugMsg:
        typeTxt = "Debug";
        break;
    case QtInfoMsg:
        typeTxt = "Info";
        break;
    case QtWarningMsg:
        typeTxt = "Warning";
        break;
    case QtCriticalMsg:
        typeTxt = "Critical";
        break;
    case QtFatalMsg:
        typeTxt = "Fatal";
        break;
    }

    QString txt = QString("%1 - Qt %2: %3\n").arg(currentLogTimestamp()).arg(typeTxt).arg(msg);

    logToLoggerStream(txt);
}

#ifdef HAVE_FFMPEG

void ffmpegLogToDiskHandler(void* ptr, int level, const char* fmt, va_list vl)
{
    char lineBuffer[1024];
    static int printPrefix = 1;

    if ((level & 0xFF) > av_log_get_level()) {
        return;
    }
    // We need to use the *previous* printPrefix value to determine whether to
    // print the prefix this time. av_log_format_line() will set the printPrefix
    // value to indicate whether the prefix should be printed *next time*.
    bool shouldPrefixThisMessage = printPrefix != 0;

    av_log_format_line(ptr, level, fmt, vl, lineBuffer, sizeof(lineBuffer), &printPrefix);

    if (shouldPrefixThisMessage) {
        QString txt = QString("%1 - FFmpeg: %2").arg(currentLogTimestamp()).arg(lineBuffer);
        logToLoggerStream(txt);
    }
    else {
        QString txt = QString(lineBuffer);
        logToLoggerStream(txt);
    }
}

#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

static UINT s_HitUnhandledException = 0;

LONG WINAPI UnhandledExceptionHandler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    // Only write a dump for the first unhandled exception
    if (InterlockedCompareExchange(&s_HitUnhandledException, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    WCHAR dmpFileName[MAX_PATH];
    swprintf_s(dmpFileName, L"%ls\\PLANK-%I64u.dmp",
               (PWCHAR)QDir::toNativeSeparators(Path::getLogDir()).utf16(), QDateTime::currentSecsSinceEpoch());
    QString qDmpFileName = QString::fromUtf16((const char16_t*)dmpFileName);
    HANDLE dumpHandle = CreateFileW(dmpFileName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpHandle != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info;

        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ExceptionInfo;
        info.ClientPointers = FALSE;

        DWORD typeFlags = MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpIgnoreInaccessibleMemory |
                MiniDumpWithUnloadedModules |
                MiniDumpWithThreadInfo;

        if (MiniDumpWriteDump(GetCurrentProcess(),
                               GetCurrentProcessId(),
                               dumpHandle,
                               (MINIDUMP_TYPE)typeFlags,
                               &info,
                               nullptr,
                               nullptr)) {
            qCritical() << "Unhandled exception! Minidump written to:" << qDmpFileName;
        }
        else {
            qCritical() << "Unhandled exception! Failed to write dump:" << GetLastError();
        }

        CloseHandle(dumpHandle);
    }
    else {
        qCritical() << "Unhandled exception! Failed to open dump file:" << qDmpFileName << "with error" << GetLastError();
    }

    // Let the program crash and WER collect a dump
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

#ifdef Q_OS_UNIX

static int signalFds[2];

void handleSignal(int sig)
{
    send(signalFds[0], &sig, sizeof(sig), 0);
}

int signalHandlerThread()
{

    bool requestedQuit = false;

    int sig;
    while (recv(signalFds[1], &sig, sizeof(sig), MSG_WAITALL) == sizeof(sig)) {
        qInfo("Received signal: %d", sig);

        switch (sig) {
        case SIGINT:
        case SIGTERM:
        {
            if (requestedQuit) {
                qInfo("Exiting immediately on second signal");
                _Exit(1);
            }

            // Qt owns dispatch throughout the Linux session. Its Quit
            // filter keeps the event loop alive until asynchronous teardown.
            QCoreApplication::postEvent(QCoreApplication::instance(), new QEvent(QEvent::Quit));
            requestedQuit = true;
            break;
        }

        default:
            Q_UNREACHABLE();
        }
    }

    return 0;
}

void configureSignalHandlers()
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) == -1) {
        qCritical("socketpair() failed: %d",
                     errno);
        return;
    }

    // Create a thread to handle our signals safely outside of signal context
    try {
        std::thread(signalHandlerThread).detach();
    } catch (const std::system_error& error) {
        qCritical("Unable to create signal handler: %s", error.what());
        close(signalFds[0]); close(signalFds[1]);
        return;
    }

    struct sigaction sa = {};
    sa.sa_handler = handleSignal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#else
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
    // Set the app version for the QCommandLineParser's showVersion() command
    QCoreApplication::setApplicationVersion(PLANK_VERSION_STR);

    // Set these here to allow us to use the default QSettings constructor and
    // establish the PLANK settings/cache namespace before paths are
    // initialized. There are no deployed legacy clients requiring migration.
    QCoreApplication::setOrganizationName("Instinctual");
    QCoreApplication::setOrganizationDomain("instinctual.la");
    QCoreApplication::setApplicationName(PlankClientIdentity::applicationName);

    if (QFile(QDir::currentPath() + "/portable.dat").exists()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::currentPath());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir::currentPath());

        // Initialize paths for portable mode
        Path::initialize(true);
    }
    else {
        // Initialize paths for standard installation
        Path::initialize(false);
    }

    // Override the default QML cache directory with the one we chose
    if (qEnvironmentVariableIsEmpty("QML_DISK_CACHE_PATH")) {
        qputenv("QML_DISK_CACHE_PATH", Path::getQmlCacheDir().toUtf8());
    }

#ifdef Q_OS_WIN32
    // Grab the original std handles before we potentially redirect them later
    HANDLE oldConOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE oldConErr = GetStdHandle(STD_ERROR_HANDLE);
#endif

#ifdef LOG_TO_FILE
    QDir logDir(Path::getLogDir());
    QString logNamePattern;

#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
    const QFileDevice::Permissions privateDirectoryPermissions =
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    bool logDirectoryReady = logDir.exists() || logDir.mkpath(".", privateDirectoryPermissions);
    if (logDirectoryReady) {
        logDirectoryReady = QFile::setPermissions(logDir.absolutePath(), privateDirectoryPermissions);
    }
    if (!logDirectoryReady) {
        QTextStream(stderr) << "Unable to create private log directory: " << logDir.absolutePath() << Qt::endl;
    }
    const QString logPrefix = QString::fromLatin1(PlankClientIdentity::executable);
    logNamePattern = logPrefix + QStringLiteral("-*.log");
    const QString logFileName = logPrefix + QString("-%1-%2.log")
                                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"))
                                    .arg(QCoreApplication::applicationPid());
#else
    logNamePattern = "PLANK-*.log";
    const QString logFileName = QString("PLANK-%1.log").arg(QDateTime::currentSecsSinceEpoch());
#endif

#ifdef Q_OS_WIN32
    // Only log to a file if the user didn't redirect stderr somewhere else
    if (IS_UNSPECIFIED_HANDLE(oldConErr))
#endif
    {
        s_LoggerFile = new QFile(logDir.filePath(logFileName));
#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
        const bool opened = logDirectoryReady &&
                            s_LoggerFile->open(QIODevice::WriteOnly | QIODevice::Text,
                                               QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#else
        const bool opened = s_LoggerFile->open(QIODevice::WriteOnly | QIODevice::Text);
#endif
        if (opened) {
            s_LoggerFileStream.setDevice(s_LoggerFile);
        }
    }
#endif

    // Register our logger with all libraries
    qInstallMessageHandler(qtLogToDiskHandler);
#ifdef HAVE_FFMPEG
    av_log_set_callback(ffmpegLogToDiskHandler);
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
    if (s_LoggerFile != nullptr && s_LoggerFile->isOpen()) {
        qInfo() << "Persistent client log:" << s_LoggerFile->fileName();
    }
#endif

#ifdef PLANK_TRANSPORT
    if (plank_transport_abi_version() != PLANK_TRANSPORT_ABI_VERSION) {
        qCritical() << "PLANK native transport ABI mismatch";
        return 9;
    }
    qInfo() << "PLANK native transport ABI"
            << plank_transport_abi_version() << "is available";
#endif

#ifdef Q_OS_WIN32
    // Create a crash dump when we crash on Windows
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
#endif

#ifdef LOG_TO_FILE
    // Prune the oldest existing logs if there are more than 10
    QStringList existingLogNames = logDir.entryList(QStringList(logNamePattern), QDir::Files, QDir::Time);
    for (int i = 10; i < existingLogNames.size(); i++) {
        qInfo() << "Removing old log file:" << existingLogNames.at(i);
        QFile(logDir.filePath(existingLogNames.at(i))).remove();
    }
#endif

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    bool forceGles;
    if (!Utils::getEnvironmentVariableOverride("FORCE_QT_GLES", &forceGles)) {
        forceGles = WMUtils::isRunningNvidiaProprietaryDriverX11() ||
                    !WMUtils::supportsDesktopGLWithEGL();
    }
    if (forceGles) {
        // The Nvidia proprietary driver causes Qt to render a black window when using
        // the default Desktop GL profile with EGL. AS a workaround, we default to
        // OpenGL ES when running on Nvidia on X11.
        // https://qt-project.atlassian.net/browse/QTBUG-106065
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGLES);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    // Use EGL consistently for the launcher and session GPU contexts; mixing
    // GLX with EGL would prevent sharing the selected presentation device.
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");

#ifdef Q_OS_WIN32
    // Let us see the true VBlank rather than DWM's approximation. We do this here
    // because this API must be called before the first swapchain (which Qt will
    // create when the window is displayed). This is supported on Win11 22H2+.
    auto fnDXGIDisableVBlankVirtualization =
        (decltype(DXGIDisableVBlankVirtualization)*)GetProcAddress(GetModuleHandleW(L"dxgi.dll"),
                                                                   "DXGIDisableVBlankVirtualization");
    if (fnDXGIDisableVBlankVirtualization) {
        fnDXGIDisableVBlankVirtualization();
    }
#endif

#ifdef Q_OS_MACOS
    // This avoids using the default keychain for SSL, which may cause
    // password prompts on macOS.
    qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
#endif

    // The qualified Qt session frontend and launcher share the GUI-thread render loop.
    qputenv("QSG_RENDER_LOOP", "basic");
#ifdef Q_OS_LINUX
    QtSessionShell::configureWaylandExposure();
#endif

#if defined(Q_OS_DARWIN) && defined(QT_DEBUG) && !defined(HAVE_LIBPLACEBO_VULKAN)
    // Enable Metal valiation for debug builds without libplacebo
    //
    // The current MoltenVK driver as of Vulkan SDK 1.4.350 triggers Metal debug layer
    // violations on frame and overlay uploads like:
    // _validateReplaceRegion:252: failed assertion `Replace Region Validation
    // bytesPerRow(4803) must be a multiple of MTLPixelFormatBGRA8Unorm pixel bytes(4).
    qputenv("MTL_DEBUG_LAYER", "1");
    qputenv("MTL_SHADER_VALIDATION", "1");
#endif

    // We don't want system proxies to apply to us
    QNetworkProxyFactory::setUseSystemConfiguration(false);

    // Clear any default application proxy
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    QNetworkProxy::setApplicationProxy(noProxy);

    // Register custom metatypes for use in signals
    qRegisterMetaType<NvApp>("NvApp");

#ifdef Q_OS_MACOS
    MacApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif

#ifdef Q_OS_MACOS
    // Our authenticated setup requires TLS1.3. Qt's SecureTransport backend
    // cannot provide it; never silently select that backend on a clean Mac.
    if (!QSslSocket::setActiveBackend(QStringLiteral("openssl")) ||
            !QSslSocket::supportedProtocols().contains(QSsl::TlsV1_3)) {
        qCritical() << "PLANK requires the bundled OpenSSL TLS1.3 backend; available:"
                    << QSslSocket::availableBackends();
        return 10;
    }
    qInfo() << "PLANK TLS backend:" << QSslSocket::activeBackend()
            << QSslSocket::sslLibraryVersionString();
#endif
    QGuiApplication::setApplicationDisplayName(PlankClientIdentity::displayName);

#ifdef Q_OS_DARWIN
    // macOS defaults "Keyboard navigation" to text fields and lists only, which
    // prevents Tab (and the gamepad navigation that synthesizes it) from moving
    // focus between non-text controls on the settings page. Force Tab to reach
    // all controls so keyboard and gamepad UI navigation work without requiring
    // the user to enable a system accessibility setting. Other platforms already
    // default to this behavior.
    app.styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
#endif

#ifdef Q_OS_UNIX
    // Marshal process shutdown into the Qt application lifecycle.
    // NB: This has to be done after the QGuiApplication is constructed to
    // ensure Qt has already installed its VT signals before we override
    // some of them with our own.
    configureSignalHandlers();
#endif

#ifdef Q_OS_WIN32
    // If we don't have stdout or stderr handles (which will normally be the case
    // since we're a /SUBSYSTEM:WINDOWS app), attach to our parent console and use
    // that for stdout and stderr.
    //
    // If we do have stdout or stderr handles, that means the user has used standard
    // handle redirection. In that case, we don't want to override those handles.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // If we didn't have an old stdout/stderr handle, use the new CONOUT$ handle
        if (IS_UNSPECIFIED_HANDLE(oldConOut)) {
            FILE* fp;
            if (freopen_s(&fp, "CONOUT$", "w", stdout) == 0) {
                setvbuf(fp, NULL, _IONBF, 0);
            }
            else {
                freopen_s(&fp, "NUL", "w", stdout);
            }
        }
        if (IS_UNSPECIFIED_HANDLE(oldConErr)) {
            FILE* fp;
            if (freopen_s(&fp, "CONOUT$", "w", stderr) == 0) {
                setvbuf(fp, NULL, _IONBF, 0);
            }
            else {
                freopen_s(&fp, "NUL", "w", stderr);
            }
        }
    }
#endif

    GlobalCommandLineParser parser;
    GlobalCommandLineParser::ParseResult commandLineParserResult = parser.parse(app.arguments());
    // Apply the initial translation based on user preference
    StreamingPreferences::get()->retranslate();

    // Trickily declare the translation for dialog buttons
    QCoreApplication::translate("QPlatformTheme", "&Yes");
    QCoreApplication::translate("QPlatformTheme", "&No");
    QCoreApplication::translate("QPlatformTheme", "OK");
    QCoreApplication::translate("QPlatformTheme", "Help");
    QCoreApplication::translate("QPlatformTheme", "Cancel");

#ifndef Q_OS_DARWIN
    // Set the window icon except on macOS where we want to keep the
    // modified macOS 11 style rounded corner icon.
    app.setWindowIcon(QIcon(":/res/plank-logo.png"));
#endif

    // Match the PLANK desktop entry so Wayland and X11 shells group
    // both the Qt launcher and session windows under the packaged identity.
    app.setDesktopFileName(PlankClientIdentity::desktopId);

    // Register our C++ types for QML
    qmlRegisterType<ComputerModel>("ComputerModel", 1, 0, "ComputerModel");
    qmlRegisterUncreatableType<InternetConnectivity>("InternetConnectivity", 1, 0,
        "InternetConnectivity", "The application owns the connectivity monitor");
    qmlRegisterUncreatableType<Session>("Session", 1, 0, "Session", "Session cannot be created from QML");
    qmlRegisterSingletonType<ComputerManager>("ComputerManager", 1, 0,
                                              "ComputerManager",
                                              [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                  return new ComputerManager(StreamingPreferences::get(qmlEngine));
                                              });
    qmlRegisterSingletonType<SystemProperties>("SystemProperties", 1, 0,
                                               "SystemProperties",
                                               [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                   return new SystemProperties();
                                               });
    qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0,
                                                   "StreamingPreferences",
                                                   [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                       return StreamingPreferences::get(qmlEngine);
                                                   });

    // Installer setup is a standalone process: use native macOS controls there
    // without changing the established Material launcher/streaming UI.
#ifdef Q_OS_MACOS
    if (commandLineParserResult == GlobalCommandLineParser::PermissionsSetupRequested)
        QQuickStyle::setStyle("macOS");
    else
#endif
        QQuickStyle::setStyle("Material");

    // Our icons are styled for a dark theme, so we do not allow the user to override this
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");

    // These are defaults that we allow the user to override
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_ACCENT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#5A9CE6");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_PRIMARY")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#393D43");
    }

    QQmlApplicationEngine engine;
    QString initialView;
    switch (commandLineParserResult) {
    case GlobalCommandLineParser::NormalStartRequested:
        initialView = "qrc:/gui/PcView.qml";
        break;
#ifdef Q_OS_MACOS
    case GlobalCommandLineParser::PermissionsSetupRequested:
        // The installer opens only the permission view, never the bookmark UI.
        break;
#endif
    case GlobalCommandLineParser::StreamRequested:
        {
            initialView = "qrc:/gui/CliStartStreamSegue.qml";
            StreamingPreferences* preferences = StreamingPreferences::get();
            StreamCommandLineParser streamParser;
            streamParser.parse(app.arguments(), preferences);
            QString host    = streamParser.getHost();
            QString appName = streamParser.getAppName();
            QString plankUsername = streamParser.getPlankUsername();
            QString plankPassword = streamParser.takePlankPassword();
            auto launcher = new CliStartStream::Launcher(
                    host, appName, preferences,
                    std::move(plankUsername),
                    std::move(plankPassword), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    }

    engine.rootContext()->setContextProperty("initialView", initialView);
    auto* internetConnectivity = new InternetConnectivity(PlankClientPolicy().internetCheck(), &engine);
    engine.rootContext()->setContextProperty("internetConnectivity", internetConnectivity);
    // Do not probe during --version or the Installer's permissions-only setup.
    if (!initialView.isEmpty()) {
        QObject::connect(&app, &QCoreApplication::aboutToQuit,
                         internetConnectivity, &InternetConnectivity::stop);
        QObject::connect(&app, &QGuiApplication::applicationStateChanged,
                         internetConnectivity, [internetConnectivity](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) internetConnectivity->refreshIfStale();
        });
        if (internetConnectivity->state() != InternetConnectivity::Disabled) {
            QNetworkInformation::loadDefaultBackend();
            if (auto* network = QNetworkInformation::instance()) {
                QObject::connect(network, &QNetworkInformation::reachabilityChanged,
                                 internetConnectivity, &InternetConnectivity::networkChanged);
                QObject::connect(network, &QNetworkInformation::transportMediumChanged,
                                 internetConnectivity, &InternetConnectivity::networkChanged);
            }
        }
        // The main window's Internet indicator owns visibility-based probing.
        // Session windows and an active application alone must not restart it.
    }
    engine.rootContext()->setContextProperty(
                "runConfigChecks",
                commandLineParserResult == GlobalCommandLineParser::NormalStartRequested);

#ifdef Q_OS_MACOS
    // Ask before any stream can capture the pointer. Also handle enabling the
    // preference later in Settings, without ever prompting from a live stream.
    auto permissions = new MacPermissions([] { return Session::get() == nullptr; }, &engine);
    engine.rootContext()->setContextProperty("macPermissions", permissions);
    auto requestKeyboardPermission = [] {
        if (Session::get() == nullptr) {
            MacKeyboardCapture::requestPermissionIfNeeded(
                        StreamingPreferences::get()->captureSysKeysMode != StreamingPreferences::CSK_OFF);
        }
    };
    QObject::connect(StreamingPreferences::get(), &StreamingPreferences::captureSysKeysModeChanged,
                     &app, requestKeyboardPermission);
    // Resolve optional microphone consent before constructing the launcher:
    // no bookmark/autoconnect can race a pending native permission dialog.
    // Never block Qt's main event loop or start recording to request consent.
    // CLI autoconnect remains non-prompting; provision via the launcher first.
    if (commandLineParserResult == GlobalCommandLineParser::NormalStartRequested ||
            commandLineParserResult == GlobalCommandLineParser::PermissionsSetupRequested) {
        const QString startupView = commandLineParserResult == GlobalCommandLineParser::PermissionsSetupRequested ?
                    QStringLiteral("qrc:/gui/MacPermissionSetup.qml") : QStringLiteral("qrc:/gui/main.qml");
        QTimer::singleShot(0, &engine, [&engine, requestKeyboardPermission, startupView] {
            requestKeyboardPermission();
            MacRawWacomInput::requestPermissionIfNeeded();
            plankMacRequestMicrophonePermission([context = QPointer<QQmlApplicationEngine>(&engine), startupView] {
                if (!context) return;
                QMetaObject::invokeMethod(context, [context, startupView] {
                    if (!context) return;
                    context->load(QUrl(startupView));
                    if (context->rootObjects().isEmpty()) QCoreApplication::exit(-1);
                }, Qt::QueuedConnection);
            });
        });
    }
    else
#endif
    {
        // Load immediately on Linux and for non-prompting CLI autoconnect.
        engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
        if (engine.rootObjects().isEmpty()) {
            return -1;
        }
        qInfo() << "Qt launcher ready; decoder qualification deferred to the stream surface";
    }

    int err = app.exec();

    // Give worker tasks time to exit cleanly before process teardown.
    QThreadPool::globalInstance()->waitForDone(30000);

    // Restore the default logger for all libraries before shutting down ours
    qInstallMessageHandler(nullptr);
#ifdef HAVE_FFMPEG
    av_log_set_callback(av_log_default_callback);
#endif

#ifdef Q_OS_WIN32
    // Ensure redirected command-line output reaches the destination file.
    fflush(stderr);
    fflush(stdout);
#endif

    return err;
}
