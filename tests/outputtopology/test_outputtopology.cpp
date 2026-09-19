#include <QtTest>

#include "outputtopology.h"
#include "../../app/streaming/macdisplaygeometry.h"

class TestOutputTopology : public QObject
{
    Q_OBJECT

private slots:
    void presentationOutputCountDuringTransitions();
    void advertisesOnlyImplementedClipboardSupport();
    void parsesQualificationVector();
    void roundTripsQualificationVector();
    void rejectsDuplicateIdentity();
    void rejectsConfiguredModeMismatch();
    void acceptsTallCinemaModes();
    void enforcesHostDisplayPolicy();
    void validatesRequestedLayoutGeometry();
    void matchesOneClientDisplay();
    void matchesPrimaryInDesktopOrder();
    void matchesTwoClientDisplaysLeftToRight();
    void rejectsUnsupportedClientLayouts();
    void parsesFixedCapture();
    void rejectsInvalidFixedCapture();
    void recognizesDescriptionCapabilities();
    void matchesMacClientCanvas();
    void matchesRetinaClientCanvas();
    void matchesMacFullscreenViewport();
    void matchesMultipleNativeFullscreenViewports();
    void buildsMacDisplayRequest();
};

void TestOutputTopology::presentationOutputCountDuringTransitions()
{
    NvOutputTopology topology;
    topology.outputs.resize(1);
    QCOMPARE(topology.outputCountForLayout("physical"), 1);
    QCOMPARE(topology.outputCountForLayout("fixed"), 1);
    // Matching two Client displays must use the requested count even before
    // the Host has replaced its previous single-output desktop.
    QCOMPARE(topology.outputCountForLayout("dual-horizontal"), 2);
    topology.outputs.resize(2);
    QCOMPARE(topology.outputCountForLayout("physical"), 2);
    QCOMPARE(topology.outputCountForLayout("fixed"), 2);
    QCOMPARE(topology.outputCountForLayout("single"), 1);
    topology.outputs.clear();
    QCOMPARE(topology.outputCountForLayout("physical"), 0);
    QCOMPARE(topology.outputCountForLayout("unresolved"), 0);
}

void TestOutputTopology::matchesMultipleNativeFullscreenViewports()
{
    for (int count : {-1, 0}) {
        QVERIFY(!MacDisplayGeometry::useNativeFullscreen(count));
    }

    // Each native Space uses its own camera inset and backing-pixel density.
    for (int count : {1, 2, 3}) {
        QVERIFY(MacDisplayGeometry::useNativeFullscreen(count));
        int height = 1329, pixels = 2658;
        QVERIFY(MacDisplayGeometry::insetTop(2056, height, 4112, pixels, 38));
        QCOMPARE(height, 1291);
        QCOMPARE(pixels, height * 2);
        height = 1440; pixels = 1440;
        QVERIFY(MacDisplayGeometry::insetTop(2560, height, 2560, pixels, 0));
        QCOMPARE(height, 1440);
        QCOMPARE(pixels, height);
    }
}

void TestOutputTopology::advertisesOnlyImplementedClipboardSupport()
{
    const auto negotiated = NvOutputTopology::SupportedFeatureFlags &
                            NvOutputTopology::ClipboardSyncFeature;
#ifdef Q_OS_MACOS
    QCOMPARE(negotiated, NvOutputTopology::ClipboardSyncFeature);
#else
    QCOMPARE(negotiated, 0);
#endif
    // A host without the feature cannot enable it on any client platform.
    QCOMPARE(NvOutputTopology::SupportedFeatureFlags &
             ~NvOutputTopology::ClipboardSyncFeature &
             NvOutputTopology::ClipboardSyncFeature, 0);
}

void TestOutputTopology::matchesMacFullscreenViewport()
{
    for (int inset : {0, 24, 34, 38}) {
        int logicalHeight = 1107;
        int pixelHeight = 2214;
        QVERIFY(MacDisplayGeometry::insetTop(1710, logicalHeight, 3420, pixelHeight, inset));
        int scale = 0;
        QString error;
        const QVector<NvClientDisplay> displays = {{QRect(-1710, inset, 1710, logicalHeight),
            QSize(3420, pixelHeight), QSize(3420, pixelHeight)}};
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(displays, &error, &scale),
                 QStringLiteral("3420x%1").arg(pixelHeight));
        QCOMPARE(scale, 2);
        QVERIFY(error.isEmpty());
        const auto request = NvOutputTopology::macDisplayRequest(
            QStringLiteral("3420x%1").arg(pixelHeight), "hevc-10-444-videotoolbox", scale);
        QVERIFY(!request.isEmpty());
    }
}

void TestOutputTopology::buildsMacDisplayRequest()
{
    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) + "/tests/protocol/macos-display-v3.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(NvOutputTopology::macDisplayRequest("3420x2214", "hevc-10-420-videotoolbox", 2), fixture);
    auto manual = fixture;
    manual["scale"] = 1;
    manual["encoding_mode"] = "hevc-10-444-videotoolbox";
    QCOMPARE(NvOutputTopology::macDisplayRequest("3420x2214", "hevc-10-444-videotoolbox", 1), manual);
    QVERIFY(NvOutputTopology::macDisplayRequest("3420x2214", "hevc-10-444-videotoolbox", 3).isEmpty());
    QVERIFY(NvOutputTopology::macDisplayRequest("3420x2214", "invalid", 2).isEmpty());
    QVERIFY(NvOutputTopology::macDisplayRequest("3421x2214", "hevc-10-444-videotoolbox", 2).isEmpty());
}

void TestOutputTopology::matchesRetinaClientCanvas()
{
    int scale = 0;
    QString error;
    // Current compositor backing pixels, not panel-native pixels. Odd logical
    // heights are valid when the actual encoded height remains even.
    const QVector<NvClientDisplay> laptop = {{QRect(0,0,1710,1107), QSize(3024,1964), QSize(3420,2214)}};
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(laptop, &error, &scale), QString("3420x2214"));
    QCOMPARE(scale, 2);
    QVERIFY(error.isEmpty());
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(3840,2160), QSize(3840,2160)},
        {QRect(-1710,0,1710,1107), QSize(3024,1964), QSize(3420,2214)}}, &error, &scale), QString("7260x2214"));
    QCOMPARE(scale, 2);
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(1920,1080), QSize(1920,1080)}}, &error, &scale), QString("1920x1080"));
    QCOMPARE(scale, 1);
    // Linux clients retain their existing native-pixel policy.
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(3840,2160)}}, &error, &scale), QString("3840x2160"));
    QCOMPARE(scale, 1);
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1710,1107), QSize(3420,2214), QSize(3420,2214)},
        {QRect(1710,0,1920,1080), QSize(1920,1080), QSize(1920,1080)}}, &error, &scale).isEmpty());
    QVERIFY(error.contains("same 1x or 2x"));
    QCOMPARE(scale, 1);
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1710,1107), QSize(3024,1964), QSize(3024,1964)}}, &error, &scale).isEmpty());
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,2500,1500), QSize(5000,3000), QSize(5000,3000)},
        {QRect(2500,0,2500,1500), QSize(5000,3000), QSize(5000,3000)}}, &error, &scale).isEmpty());
}

void TestOutputTopology::recognizesDescriptionCapabilities()
{
    const int version = NvOutputTopology::ProtocolVersion;
    const int fixed = NvOutputTopology::FixedCaptureFlags;
    const int linuxFlags = NvOutputTopology::SupportedFeatureFlags;
    QVERIFY(NvOutputTopology::supportsDescription(version, fixed));
    QVERIFY(NvOutputTopology::supportsDescription(version, linuxFlags));
    QVERIFY(!NvOutputTopology::supportsDescription(version - 1, fixed));
    QVERIFY(!NvOutputTopology::supportsDescription(version + 1, linuxFlags));
    QVERIFY(!NvOutputTopology::supportsDescription(version, 0));
    QVERIFY(!NvOutputTopology::supportsDescription(version, NvOutputTopology::OutputTopologyFeature));
    QVERIFY(!NvOutputTopology::supportsDescription(version, fixed ^ NvOutputTopology::HostLayoutMetadataFeature));
    QVERIFY(!NvOutputTopology::supportsDescription(version, fixed | NvOutputTopology::SelectedOutputFeature));
    QCOMPARE(NvOutputTopology::hostPlatform(version, fixed), 2);
    QCOMPARE(NvOutputTopology::hostPlatform(version, linuxFlags), 1);
    QCOMPARE(NvOutputTopology::hostPlatform(version, 0), 0);
    QCOMPARE(NvOutputTopology::hostPlatform(version - 1, fixed), 0);
}

void TestOutputTopology::matchesMacClientCanvas()
{
    // Pixel dimensions are independent of logical compositor scaling.
    for (QSize logical : {QSize(3840,2160), QSize(3072,1728), QSize(1920,1080)}) {
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({{QRect(QPoint(-100,0), logical), QSize(3840,2160)}}), QString("3840x2160"));
    }
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({{QRect(0,0,5120,2160), QSize(5120,2160)}}), QString("5120x2160"));
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(2560,0,2560,2160), QSize(2560,2160)},
        {QRect(0,0,2560,2160), QSize(2560,2160)}}), QString("5120x2160"));
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,3840,2160), QSize(3840,2160)},
        {QRect(3840,0,3840,2160), QSize(3840,2160)}}), QString("7680x2160"));
    for (QSize native : {QSize(3024,1964), QSize(3456,2234), QSize(2880,1864), QSize(2160,3840)}) {
        const QString mode = QStringLiteral("%1x%2").arg(native.width()).arg(native.height());
        const QVector<NvClientDisplay> screens = {{QRect(QPoint(0,0), native / 2), native}};
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(screens), mode);
        QCOMPARE(NvOutputTopology::macDisplayModeSize(mode), native);
        QVERIFY(!NvOutputTopology::virtualModeSize(mode).isValid());
        QString layout;
        QStringList modes;
        QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(screens, layout, modes));
    }
    for (const QString mode : {"0x2160", "3023x1964", "3024x1963", "8194x2160",
            "3840x8194", "-2x2", "03024x1964", "3024X1964", "3024x1964x2", "2x+2",
            "2x2 ", "9999999999999999999x2"}) {
        QVERIFY(!NvOutputTopology::macDisplayModeSize(mode).isValid());
    }
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,5120,2160), QSize(5120,2160)},
        {QRect(5120,0,5120,2160), QSize(5120,2160)}}).isEmpty());
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(1920,1080)},
        {QRect(0,1080,1920,1080), QSize(1920,1080)}}).isEmpty());
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({}).isEmpty());
}

static QJsonObject fixedCaptureFixture()
{
    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) + "/tests/protocol/fixed-capture-v13.json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void TestOutputTopology::parsesFixedCapture()
{
    const auto fixture = fixedCaptureFixture();
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(fixture, topology));
    QCOMPARE(topology.featureFlags, NvOutputTopology::FixedCaptureFlags);
    QCOMPARE(topology.desktopWidth, 3840);
    QCOMPARE(topology.desktopHeight, 2160);
    QCOMPARE(topology.captureLogicalBounds, QRectF(-1920, 0, 1920, 1080));
    QCOMPARE(topology.outputs.size(), 1);
    QCOMPARE(topology.toJson(), fixture);
    QVERIFY(!(topology.featureFlags & NvOutputTopology::UnifiedAbsoluteInputFeature));
    QVERIFY(!(topology.featureFlags & NvOutputTopology::SessionTakeoverFeature));
    QVERIFY(!(NvOutputTopology::SupportedFeatureFlags & NvOutputTopology::FixedCaptureFeature));
    // Reuse is atomic, including failures; never retain stale logical bounds.
    QVERIFY(!NvOutputTopology::fromJson({}, topology));
    QCOMPARE(topology.toJson(), fixture);
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    for (QSize points : {QSize(3840, 2160), QSize(2560, 1440)}) {
        auto varied = fixture;
        auto capture = varied["capture"].toObject();
        capture["logical_bounds"] = QJsonObject {{"x", 100}, {"y", -500},
            {"width", points.width()}, {"height", points.height()}};
        varied["capture"] = capture;
        QVERIFY(NvOutputTopology::fromJson(varied, topology));
        QCOMPARE(topology.captureLogicalBounds, QRectF(QPointF(100, -500), points));
        QCOMPARE(topology.desktopWidth, 3840);
        QCOMPARE(topology.toJson(), varied);
    }
}

void TestOutputTopology::rejectsInvalidFixedCapture()
{
    const auto fixture = fixedCaptureFixture();
    QVERIFY(!fixture.isEmpty());
    NvOutputTopology topology;
    for (QJsonValue bad : {QJsonValue(-1), QJsonValue(0), QJsonValue(3), QJsonValue(8194),
                          QJsonValue(1e99), QJsonValue(3840.5), QJsonValue("3840")}) {
        auto object = fixture;
        auto capture = object["capture"].toObject();
        capture["width"] = bad; object["capture"] = capture;
        QVERIFY(!NvOutputTopology::fromJson(object, topology));
    }
    for (const char* field : {"rgb_identity", "chroma", "range", "encoding_mode", "transfer"}) {
        auto object = fixture;
        auto capture = object["capture"].toObject();
        auto profile = capture["encoding_profile"].toObject();
        profile[field] = "incorrect"; capture["encoding_profile"] = profile; object["capture"] = capture;
        QVERIFY(!NvOutputTopology::fromJson(object, topology));
    }
    auto object = fixture;
    object["feature_flags"] = NvOutputTopology::FixedCaptureFlags | NvOutputTopology::SessionTakeoverFeature;
    QVERIFY(!NvOutputTopology::fromJson(object, topology));
    object = fixture; object["generation"] = "not-a-generation";
    QVERIFY(!NvOutputTopology::fromJson(object, topology));
    object = fixture; object["layout"] = QJsonObject();
    QVERIFY(!NvOutputTopology::fromJson(object, topology));
    for (QJsonValue bad : {QJsonValue(0), QJsonValue(-1), QJsonValue(1e99), QJsonValue("1920")}) {
        object = fixture;
        auto capture = object["capture"].toObject();
        auto logical = capture["logical_bounds"].toObject();
        logical["width"] = bad; capture["logical_bounds"] = logical; object["capture"] = capture;
        QVERIFY(!NvOutputTopology::fromJson(object, topology));
    }
}

void TestOutputTopology::parsesQualificationVector()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "PLANK_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());

    NvOutputTopology topology;
    QString error;
    QVERIFY2(NvOutputTopology::fromJson(document.object(), topology, &error), qPrintable(error));
    QCOMPARE(topology.outputs.size(), 2);
    QCOMPARE(topology.desktopWidth, 5120);
    QCOMPARE(topology.featureFlags & NvOutputTopology::SupportedFeatureFlags, 524287);
    QVERIFY((topology.featureFlags & NvOutputTopology::CaptureSourceSelectionFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::EncoderBackendSelectionFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::NvfbcHevc10NvencFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::FixedTransportMtuFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::SessionTakeoverFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::TopologyGenerationFeature) != 0);
    QVERIFY(!topology.generation.isEmpty());
    QCOMPARE(topology.layoutKind, QString("dual-horizontal"));
    QCOMPARE(topology.virtualModes,
             QStringList({QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));
    QVERIFY(topology.virtualLayout);
    QCOMPARE(topology.startupLayoutKind, QStringLiteral("physical"));
    QCOMPARE(topology.allowedLayoutKinds,
             QStringList({QStringLiteral("physical"), QStringLiteral("single"),
                          QStringLiteral("dual-horizontal")}));
    QCOMPARE(topology.outputs.at(0).configuredMode, QString("3840x2160"));
    QCOMPARE(topology.outputs.at(1).configuredMode, QString("1280x2160"));
    QCOMPARE(topology.outputs.at(1).sourceX, 3840);
}

void TestOutputTopology::roundTripsQualificationVector()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "PLANK_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(document.object(), topology));

    NvOutputTopology restored;
    QVERIFY(NvOutputTopology::fromJson(topology.toJson(), restored));
    QCOMPARE(restored.toJson(), topology.toJson());
}

void TestOutputTopology::rejectsDuplicateIdentity()
{
    QJsonObject output {
        {"id", "x11:DP-2"}, {"name", "DP-2"}, {"x", 0}, {"y", 0},
        {"width", 3840}, {"height", 2160}, {"rotation", 0},
        {"refresh_millihz", 60000}, {"primary", true}, {"virtual", true},
        {"configured_mode", "3840x2160"},
        {"source_rect", QJsonObject {{"x", 0}, {"y", 0},
                                      {"width", 3840}, {"height", 2160}}},
    };
    QJsonObject document {
        {"schema_version", 12}, {"feature_flags", 65535}, {"generation", "test"},
        {"layout", QJsonObject {{"kind", "dual-horizontal"}, {"virtual", true},
                                 {"virtual_modes", QJsonArray {"3840x2160", "3840x2160"}},
                                 {"output_count", 2}, {"startup_kind", "physical"},
                                 {"allowed_kinds", QJsonArray {
                                      "physical", "single", "dual-horizontal"}}}},
        {"desktop", QJsonObject {{"x", 0}, {"y", 0}, {"width", 7680}, {"height", 2160}}},
        {"outputs", QJsonArray {output, output}},
    };
    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

void TestOutputTopology::rejectsConfiguredModeMismatch()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    QJsonArray outputs = document.value("outputs").toArray();
    QJsonObject second = outputs.at(1).toObject();
    second["configured_mode"] = QStringLiteral("1024x2160");
    outputs[1] = second;
    document["outputs"] = outputs;

    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

void TestOutputTopology::acceptsTallCinemaModes()
{
    const QStringList modes = NvOutputTopology::qualifiedVirtualModes();
    QCOMPARE(modes.size(), 12);
    QCOMPARE(modes.at(6), QStringLiteral("2560x2160"));
    QCOMPARE(modes.at(9), QStringLiteral("3840x2160"));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("1024x2160")),
             QSize(1024, 2160));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("4096x2160")),
             QSize(4096, 2160));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("5120x2160")),
             QSize(5120, 2160));
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("1280x720")).isValid());
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("1280x1024")).isValid());
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("single"), {QStringLiteral("2560x2160")}),
             QSize(2560, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}),
             QSize(5120, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("4096x2160"), QStringLiteral("1024x2160")}),
             QSize(5120, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("4096x2160"), QStringLiteral("1280x2160")}),
             QSize(5376, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("4096x2160"), QStringLiteral("4096x2160")}),
             QSize(8192, 2160));
    QVERIFY(!NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("3840x2160"), QStringLiteral("5120x2160")}).isValid());
}

void TestOutputTopology::enforcesHostDisplayPolicy()
{
    NvOutputTopology topology;
    QVERIFY(!topology.displayPolicyKnown());
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));

    topology.schemaVersion = NvOutputTopology::ProtocolVersion;
    topology.layoutKind = NvOutputTopology::PhysicalHostLayout;
    topology.startupLayoutKind = NvOutputTopology::PhysicalHostLayout;
    topology.allowedLayoutKinds = {
        QString::fromLatin1(NvOutputTopology::PhysicalHostLayout),
        QString::fromLatin1(NvOutputTopology::SingleHostLayout),
        QString::fromLatin1(NvOutputTopology::DualHorizontalHostLayout)
    };
    topology.virtualLayout = false;
    QVERIFY(topology.displayPolicyKnown());
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));

    topology.layoutKind = NvOutputTopology::SingleHostLayout;
    topology.startupLayoutKind = NvOutputTopology::SingleHostLayout;
    topology.allowedLayoutKinds = {
        QString::fromLatin1(NvOutputTopology::SingleHostLayout),
        QString::fromLatin1(NvOutputTopology::DualHorizontalHostLayout)
    };
    topology.virtualLayout = true;
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("dual-horizontal")));
}

void TestOutputTopology::validatesRequestedLayoutGeometry()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();

    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(document, topology));
    QVERIFY(topology.matchesRequestedHostLayout(
                QStringLiteral("dual-horizontal"),
                {QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));

    QJsonArray outputs = document.value("outputs").toArray();
    QJsonObject second = outputs.at(1).toObject();
    second["x"] = 2560;
    second["source_rect"] = QJsonObject {
        {"x", 2560}, {"y", 0}, {"width", 1280}, {"height", 2160}
    };
    outputs[1] = second;
    document["outputs"] = outputs;
    document["desktop"] = QJsonObject {
        {"x", 0}, {"y", 0}, {"width", 3840}, {"height", 2160}
    };

    QVERIFY(NvOutputTopology::fromJson(document, topology));
    QVERIFY(!topology.matchesRequestedHostLayout(
                QStringLiteral("dual-horizontal"),
                {QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));
}

void TestOutputTopology::matchesOneClientDisplay()
{
    QString layout;
    QStringList modes;
    QString error;
    const QVector<NvClientDisplay> displays {
        {QRect(0, 0, 3840, 2160), QSize(3840, 2160)},
    };
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(
                 displays, layout, modes, &error),
             qPrintable(error));
    QCOMPARE(layout, QStringLiteral("single"));
    QCOMPARE(modes, QStringList({QStringLiteral("3840x2160")}));
}

void TestOutputTopology::matchesPrimaryInDesktopOrder()
{
    QFile fixture(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) +
                  "/tests/protocol/output-topology-v13-virtual-primary.json");
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(QJsonDocument::fromJson(fixture.readAll()).object(), topology));
    QVERIFY(topology.featureFlags & NvOutputTopology::VirtualPrimaryConnectorFeature);
    QVERIFY(topology.matchesRequestedHostLayout("dual-horizontal", {"1920x1200", "2560x1440"}));
    QCOMPARE(topology.outputs[0].id, QStringLiteral("x11:DP-2"));
    QCOMPARE(topology.outputs[1].id, QStringLiteral("x11:DP-0"));
    QVERIFY(topology.outputs[1].primary);

    const NvClientDisplay eizo {QRect(1920, 0, 2560, 1440), QSize(2560, 1440), {}, true};
    const NvClientDisplay laptop {QRect(0, 0, 1920, 1200), QSize(3456, 2234), {}, false};
    QCOMPARE(NvOutputTopology::clientPrimaryIndex({eizo, laptop}), 1);
    QCOMPARE(NvOutputTopology::clientPrimaryIndex({laptop, eizo}), 1);
    auto leftPrimary = laptop;
    leftPrimary.primary = true;
    QCOMPARE(NvOutputTopology::clientPrimaryIndex({leftPrimary, eizo}), -1);
    auto noPrimary = eizo;
    noPrimary.primary = false;
    QCOMPARE(NvOutputTopology::clientPrimaryIndex({laptop, noPrimary}), -1);

    // A qualified virtual pair retains its existing sizes and gains only the
    // optional primary connector index for a Host that advertises the bit.
    QString layout;
    QStringList modes;
    QString error;
    int primary = -1;
    const NvClientDisplay virtualLaptop {QRect(0, 0, 1920, 1200), QSize(1920, 1200), {}, false};
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(
                 {eizo, virtualLaptop}, layout, modes, &error, &primary), qPrintable(error));
    QCOMPARE(layout, QStringLiteral("dual-horizontal"));
    QCOMPARE(modes, QStringList({QStringLiteral("1920x1200"), QStringLiteral("2560x1440")}));
    QCOMPARE(primary, 1);
    QVERIFY(NvOutputTopology::SupportedFeatureFlags &
            NvOutputTopology::VirtualPrimaryConnectorFeature);
    QVERIFY(!(NvOutputTopology::VirtualPrimaryConnectorFeature &
              NvOutputTopology::ClipboardSyncFeature));
}

void TestOutputTopology::matchesTwoClientDisplaysLeftToRight()
{
    QString layout;
    QStringList modes;
    QString error;
    QVector<NvClientDisplay> displays {
        {QRect(3840, 0, 1280, 2160), QSize(1280, 2160)},
        {QRect(0, 0, 3840, 2160), QSize(3840, 2160)},
    };
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(
                 displays, layout, modes, &error), qPrintable(error));
    QCOMPARE(layout, QStringLiteral("dual-horizontal"));
    QCOMPARE(modes, QStringList({QStringLiteral("3840x2160"),
                                 QStringLiteral("1280x2160")}));
}

void TestOutputTopology::rejectsUnsupportedClientLayouts()
{
    QString layout;
    QStringList modes;
    QString error;
    const QVector<NvClientDisplay> threeDisplays {
        {QRect(0, 0, 1920, 1080), QSize(1920, 1080)},
        {QRect(1920, 0, 1920, 1080), QSize(1920, 1080)},
        {QRect(3840, 0, 1920, 1080), QSize(1920, 1080)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                threeDisplays, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("one or two")));

    const QVector<NvClientDisplay> verticalDisplays {
        {QRect(0, 0, 1920, 1080), QSize(1920, 1080)},
        {QRect(0, 1080, 1920, 1080), QSize(1920, 1080)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                verticalDisplays, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("left to right")));

    const QVector<NvClientDisplay> unsupportedDisplay {
        {QRect(0, 0, 1280, 1024), QSize(1280, 1024)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                unsupportedDisplay, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("not a qualified PLANK virtual mode")));
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
