#include "commandlineparser.h"

#include <QCommandLineParser>
#include <QFile>
#include <QRegularExpression>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_UNIX)
#include <termios.h>
#include <unistd.h>
#endif

class StdinEchoGuard
{
public:
    StdinEchoGuard()
    {
#if defined(Q_OS_WIN)
        m_StdinHandle = GetStdHandle(STD_INPUT_HANDLE);
        if (m_StdinHandle != INVALID_HANDLE_VALUE &&
                GetConsoleMode(m_StdinHandle, &m_OriginalMode) &&
                SetConsoleMode(m_StdinHandle, m_OriginalMode & ~ENABLE_ECHO_INPUT)) {
            m_Disabled = true;
        }
#elif defined(Q_OS_UNIX)
        if (isatty(fileno(stdin)) && tcgetattr(fileno(stdin), &m_OriginalMode) == 0) {
            struct termios noEchoMode = m_OriginalMode;
            noEchoMode.c_lflag &= ~ECHO;
            if (tcsetattr(fileno(stdin), TCSAFLUSH, &noEchoMode) == 0) {
                m_Disabled = true;
            }
        }
#endif
    }

    ~StdinEchoGuard()
    {
        if (!m_Disabled) {
            return;
        }
#if defined(Q_OS_WIN)
        SetConsoleMode(m_StdinHandle, m_OriginalMode);
#elif defined(Q_OS_UNIX)
        tcsetattr(fileno(stdin), TCSAFLUSH, &m_OriginalMode);
#endif
        fputc('\n', stderr);
    }

private:
    bool m_Disabled = false;
#if defined(Q_OS_WIN)
    HANDLE m_StdinHandle = INVALID_HANDLE_VALUE;
    DWORD m_OriginalMode = 0;
#elif defined(Q_OS_UNIX)
    struct termios m_OriginalMode {};
#endif
};

static bool inRange(int value, int min, int max)
{
    return value >= min && value <= max;
}

// This method returns key's value from QMap where the key is a QString.
// Key matching is case insensitive.
template <typename T>
static T mapValue(QMap<QString, T> map, QString key)
{
    for(auto& item : map.toStdMap()) {
        if (QString::compare(item.first, key, Qt::CaseInsensitive) == 0) {
            return item.second;
        }
    }
    return T();
}

class CommandLineParser : public QCommandLineParser
{
public:
    enum MessageType {
        Info,
        Error
    };

    void setupCommonOptions()
    {
        setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        addHelpOption();
        addVersionOption();
    }

    void handleHelpAndVersionOptions()
    {
        if (isSet("help")) {
            showInfo(helpText());
        }
        if (isSet("version")) {
            showVersion();
        }
    }

    void handleUnknownOptions()
    {
        if (!unknownOptionNames().isEmpty()) {
            showError(QString("Unknown options: %1").arg(unknownOptionNames().join(", ")));
        }
    }

    void showMessage(QString message, MessageType type) const
    {
    #if defined(Q_OS_WIN32)
        UINT flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
        flags |= (type == Info ? MB_ICONINFORMATION : MB_ICONERROR);
        QString title = "PLANK";
        MessageBoxW(nullptr, reinterpret_cast<const wchar_t *>(message.utf16()),
                    reinterpret_cast<const wchar_t *>(title.utf16()), flags);
    #endif
        message = message.endsWith('\n') ? message : message + '\n';
        fputs(qPrintable(message), type == Info ? stdout : stderr);
    }

    [[ noreturn ]] void showInfo(QString message) const
    {
        showMessage(message, Info);
        exit(0);
    }

    [[ noreturn ]] void showError(QString message) const
    {
        showMessage(message + "\n\n" + helpText(), Error);
        exit(1);
    }

    int getIntOption(QString name) const
    {
        bool ok;
        int intValue = value(name).toInt(&ok);
        if (!ok) {
            showError(QString("Invalid %1 value: %2").arg(name, value(name)));
        }
        return intValue;
    }

    bool getToggleOptionValue(QString name, bool defaultValue) const
    {
        QRegularExpression re(QString("^(%1|no-%1)$").arg(name));
        QStringList options = optionNames().filter(re);
        if (options.isEmpty()) {
            return defaultValue;
        } else {
            return options.last() == name;
        }
    }

    QString getChoiceOptionValue(QString name) const
    {
        if (!m_Choices[name].contains(value(name), Qt::CaseInsensitive)) {
            showError(QString("Invalid %1 choice: %2").arg(name, value(name)));
        }
        return value(name);
    }

    void addFlagOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Use %1.").arg(descriptiveName)));
    }

    void addToggleOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Use %1.").arg(descriptiveName)));
        addOption(QCommandLineOption("no-" + name, QString("Do not use %1.").arg(descriptiveName)));
    }

    void addValueOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Specify %1 to use.").arg(descriptiveName), name));
    }

    void addChoiceOption(QString name, QString descriptiveName, QStringList choices)
    {
        addOption(QCommandLineOption(name, QString("Select %1: %2.").arg(descriptiveName, choices.join('/')), name));
        m_Choices[name] = choices;
    }

private:
    QMap<QString, QStringList> m_Choices;
};

GlobalCommandLineParser::GlobalCommandLineParser()
{
}

GlobalCommandLineParser::~GlobalCommandLineParser()
{
}

GlobalCommandLineParser::ParseResult GlobalCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Starts PLANK normally if no arguments are given.\n"
        "\n"
        "Available actions:\n"
        "  stream          Start a workstation session\n"
        "\n"
        "See 'plank-client <action> --help' for help of specific action."
    );
    parser.addPositionalArgument("action", "Action to execute", "<action>");
#ifdef Q_OS_MACOS
    parser.addOption(QCommandLineOption("setup-permissions", "Review macOS permissions without opening bookmarks or connecting."));
#endif
    parser.parse(args);
    auto posArgs = parser.positionalArguments();

#ifdef Q_OS_MACOS
    if (parser.isSet("setup-permissions")) {
        parser.handleUnknownOptions();
        if (!posArgs.isEmpty()) parser.showError("Permission setup cannot be combined with a stream action");
        parser.handleHelpAndVersionOptions();
        return PermissionsSetupRequested;
    }
#endif

    if (posArgs.isEmpty()) {
        // This method will not return and terminates the process if --version
        // or --help is specified
        parser.handleHelpAndVersionOptions();
        parser.handleUnknownOptions();
        return NormalStartRequested;
    }
    else {
        // If users supply arguments that accept values prior to the "stream"
        // positional argument, we will not be able to correctly
        // parse the value out of the input because this QCommandLineParser
        // doesn't know about all of the options that "stream" can accept.
        // To work around this issue, we just look
        // for the "stream" positional argument anywhere.
        for (int i = 0; i < posArgs.size(); i++) {
            QString action = posArgs.at(i).toLower();
            if (action == "stream") {
                return StreamRequested;
            }
        }

        parser.showError("Invalid action");
    }
}

StreamCommandLineParser::StreamCommandLineParser()
{
    m_WindowModeMap = {
        {"windowed",   StreamingPreferences::WM_WINDOWED},
        {"borderless", StreamingPreferences::WM_FULLSCREEN_DESKTOP},
    };
    m_AudioConfigMap = {
        {"stereo",       StreamingPreferences::AC_STEREO},
        {"5.1-surround", StreamingPreferences::AC_51_SURROUND},
        {"7.1-surround", StreamingPreferences::AC_71_SURROUND},
    };
    m_CaptureSysKeysModeMap = {
        {"never",      StreamingPreferences::CSK_OFF},
        {"fullscreen", StreamingPreferences::CSK_FULLSCREEN},
        {"always",     StreamingPreferences::CSK_ALWAYS},
    };
}

StreamCommandLineParser::~StreamCommandLineParser()
{
    m_PlankPassword.fill(QChar('\0'));
    m_PlankPassword.clear();
}

void StreamCommandLineParser::parse(const QStringList &args, StreamingPreferences *preferences)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Starts directly streaming a given app."
    );
    parser.addPositionalArgument("stream", "Start stream");

    // Add other arguments and options
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");
    parser.addPositionalArgument("app", "App to stream", "\"<app>\"");

    parser.addToggleOption("vsync", "V-Sync");
    parser.addValueOption("fps", "FPS");
    parser.addChoiceOption("display-mode", "display mode", m_WindowModeMap.keys());
    parser.addChoiceOption("audio-config", "audio config", m_AudioConfigMap.keys());
    parser.addToggleOption("audio-on-host", "audio on host PC");
    parser.addToggleOption("mute-on-focus-loss", "mute audio when PLANK Client window loses focus");
    parser.addToggleOption("keep-awake", "prevent display sleep while streaming");
    parser.addToggleOption("performance-overlay", "show performance overlay");
    parser.addChoiceOption("capture-system-keys", "capture system key combos", m_CaptureSysKeysModeMap.keys());
    parser.addValueOption("plank-user", "PLANK workstation username");
    parser.addFlagOption("plank-password-stdin",
                         "PLANK password read from standard input");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // Resolve --fps option
    if (parser.isSet("fps")) {
        preferences->fps = parser.getIntOption("fps");
        if (!inRange(preferences->fps, 10, 480)) {
            fprintf(stderr, "Warning: FPS is out of the supported range (10 - 480 FPS). Performance may suffer!\n");
        }
    }

    // Resolve --display option
    if (parser.isSet("display-mode")) {
        preferences->windowMode = mapValue(m_WindowModeMap, parser.getChoiceOptionValue("display-mode"));
    }

    // Resolve --vsync and --no-vsync options
    preferences->enableVsync = parser.getToggleOptionValue("vsync", preferences->enableVsync);

    // Resolve --audio-config option
    if (parser.isSet("audio-config")) {
        preferences->audioConfig = mapValue(m_AudioConfigMap, parser.getChoiceOptionValue("audio-config"));
    }


    // Resolve --audio-on-host and --no-audio-on-host options
    preferences->playAudioOnHost = parser.getToggleOptionValue("audio-on-host", preferences->playAudioOnHost);

    // Resolve --mute-on-focus-loss and --no-mute-on-focus-loss options
    preferences->muteOnFocusLoss = parser.getToggleOptionValue("mute-on-focus-loss", preferences->muteOnFocusLoss);

    // Resolve --keep-awake and --no-keep-awake options
    preferences->keepAwake = parser.getToggleOptionValue("keep-awake", preferences->keepAwake);

    // Resolve --performance-overlay and --no-performance-overlay options
    preferences->showPerformanceOverlay = parser.getToggleOptionValue("performance-overlay", preferences->showPerformanceOverlay);

    // Resolve --capture-system-keys option
    if (parser.isSet("capture-system-keys")) {
        preferences->captureSysKeysMode = mapValue(m_CaptureSysKeysModeMap, parser.getChoiceOptionValue("capture-system-keys"));
    }

    const bool hasPlankUser = parser.isSet("plank-user");
    const bool hasPlankPassword = parser.isSet("plank-password-stdin");
    if (hasPlankUser != hasPlankPassword) {
        parser.showError("--plank-user and --plank-password-stdin must be used together");
    }
    if (hasPlankUser) {
        m_PlankUsername = parser.value("plank-user");
        if (m_PlankUsername.isEmpty()) {
            parser.showError("PLANK username must not be empty");
        }

        QFile passwordInput;
        if (!passwordInput.open(stdin, QIODevice::ReadOnly)) {
            parser.showError("Unable to read the PLANK password from standard input");
        }
        QByteArray passwordBytes;
        {
            StdinEchoGuard echoGuard;
            passwordBytes = passwordInput.readLine(4098);
        }
        if (passwordBytes.endsWith('\n')) {
            passwordBytes.chop(1);
        }
        if (passwordBytes.endsWith('\r')) {
            passwordBytes.chop(1);
        }
        if (passwordBytes.isEmpty() || passwordBytes.size() > 4096) {
            passwordBytes.fill('\0');
            parser.showError("PLANK password must contain between 1 and 4096 bytes");
        }
        m_PlankPassword = QString::fromUtf8(passwordBytes);
        passwordBytes.fill('\0');
    }

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that both host and app has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);

    if (posArgs.length() < 3) {
        parser.showError("App not provided");
    }
    m_AppName = parser.positionalArguments().at(2);
}

QString StreamCommandLineParser::getHost() const
{
    return m_Host;
}

QString StreamCommandLineParser::getAppName() const
{
    return m_AppName;
}

QString StreamCommandLineParser::getPlankUsername() const
{
    return m_PlankUsername;
}

QString StreamCommandLineParser::takePlankPassword()
{
    return std::move(m_PlankPassword);
}
