#include "outputtopology.h"

#include <algorithm>
#include <tuple>

#include <QJsonArray>
#include <QUuid>
#include <cmath>

const char* NvOutputTopology::NativeScalingMode = "native";
const char* NvOutputTopology::ScaledSpanMode = "scaled-span";
const char* NvOutputTopology::MatchClientHostLayout = "match-client";
const char* NvOutputTopology::PhysicalHostLayout = "physical";
const char* NvOutputTopology::SingleHostLayout = "single";
const char* NvOutputTopology::DualHorizontalHostLayout = "dual-horizontal";

bool NvOutputTopology::supportsDescription(int version, int featureFlags)
{
    if (version != ProtocolVersion) return false;
    if (featureFlags & FixedCaptureFeature) return featureFlags == FixedCaptureFlags;
    const int linuxDescription = OutputTopologyFeature | SelectedOutputFeature |
            UnifiedAbsoluteInputFeature;
    return (featureFlags & linuxDescription) == linuxDescription;
}

namespace {
QJsonObject applePreviewProfile(const QString& mode)
{
    const bool fullChroma = mode == QLatin1String("hevc-10-444-videotoolbox");
    if (!fullChroma && mode != QLatin1String("hevc-10-420-videotoolbox")) return {};
    return {{"capture_source", "screencapturekit"}, {"encoder_backend", "videotoolbox"},
            {"encoding_mode", mode}, {"codec", "hevc"},
            {"profile", fullChroma ? "rext" : "main10"}, {"bit_depth", 10}, {"chroma", fullChroma ? "4:4:4" : "4:2:0"},
            {"range", "full"}, {"matrix", "bt709"}, {"primaries", "bt709"},
            {"transfer", "srgb"}, {"rgb_identity", false}};
}

bool parseFixedCapture(const QJsonObject& object, NvOutputTopology& result)
{
    if (object.size() != 4 || object.value("schema_version") != QJsonValue(NvOutputTopology::ProtocolVersion) ||
            object.value("feature_flags") != QJsonValue(NvOutputTopology::FixedCaptureFlags) ||
            !object.value("capture").isObject()) return false;
    const QString generation = object.value("generation").toString();
    if (QUuid(generation).isNull() || QUuid(generation).toString(QUuid::WithoutBraces) != generation) return false;
    const auto capture = object.value("capture").toObject();
    const QString id = capture.value("id").toString();
    const QString encodingMode = capture.value("encoding_profile").toObject().value("encoding_mode").toString();
    const auto profile = applePreviewProfile(encodingMode);
    if (capture.size() != 5 || id.isEmpty() || id.size() > 128 || !capture.value("logical_bounds").isObject() ||
            profile.isEmpty() || capture.value("encoding_profile") != QJsonValue(profile)) return false;
    auto dimension = [&capture](const char* key) {
        const QJsonValue value = capture.value(key);
        if (!value.isDouble()) return 0;
        const double number = value.toDouble();
        if (!std::isfinite(number) || number < 2 || number > 8192 || std::floor(number) != number) return 0;
        const int integer = static_cast<int>(number);
        return integer % 2 == 0 ? integer : 0;
    };
    const int width = dimension("width"), height = dimension("height");
    if (!width || !height) return false;
    const auto bounds = capture.value("logical_bounds").toObject();
    if (bounds.size() != 4) return false;
    for (const char* key : {"x", "y", "width", "height"}) {
        const auto value = bounds.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 65536) return false;
    }
    const QRectF logical(bounds.value("x").toDouble(), bounds.value("y").toDouble(),
                         bounds.value("width").toDouble(), bounds.value("height").toDouble());
    if (logical.width() <= 0 || logical.height() <= 0) return false;
    NvOutputTopology parsed;
    parsed.schemaVersion = NvOutputTopology::ProtocolVersion;
    parsed.featureFlags = NvOutputTopology::FixedCaptureFlags;
    parsed.generation = generation;
    parsed.desktopWidth = width;
    parsed.desktopHeight = height;
    parsed.layoutKind = parsed.startupLayoutKind = QStringLiteral("fixed");
    parsed.allowedLayoutKinds = {QStringLiteral("fixed")};
    parsed.captureLogicalBounds = logical;
    parsed.appleEncodingMode = encodingMode;
    NvOutput output;
    output.id = id;
    output.name = QStringLiteral("Current capture display");
    output.primary = true;
    output.width = output.sourceWidth = width;
    output.height = output.sourceHeight = height;
    parsed.outputs.append(output);
    result = parsed;
    return true;
}

bool requireInteger(const QJsonObject& object, const char* name, int& value)
{
    const QJsonValue field = object.value(name);
    if (!field.isDouble()) {
        return false;
    }
    const double number = field.toDouble();
    value = static_cast<int>(number);
    return number == value;
}

bool validLayoutKind(const QString& kind)
{
    return kind == NvOutputTopology::PhysicalHostLayout ||
            kind == NvOutputTopology::SingleHostLayout ||
            kind == NvOutputTopology::DualHorizontalHostLayout;
}

}

QStringList NvOutputTopology::qualifiedVirtualModes()
{
    return {QStringLiteral("1024x2160"), QStringLiteral("1280x2160"),
            QStringLiteral("1920x1080"), QStringLiteral("1920x1200"),
            QStringLiteral("2560x1440"), QStringLiteral("2560x1600"),
            QStringLiteral("2560x2160"),
            QStringLiteral("3440x1440"), QStringLiteral("3840x1600"),
            QStringLiteral("3840x2160"), QStringLiteral("4096x2160"),
            QStringLiteral("5120x2160")};
}

QSize NvOutputTopology::virtualModeSize(const QString& mode)
{
    const QStringList parts = mode.split(QLatin1Char('x'));
    if (!qualifiedVirtualModes().contains(mode) || parts.size() != 2) {
        return QSize();
    }
    return QSize(parts[0].toInt(), parts[1].toInt());
}

QSize NvOutputTopology::virtualCanvasSize(const QString& hostLayout,
                                          const QStringList& virtualModes)
{
    const QSize first = virtualModeSize(virtualModes.value(0));
    if (hostLayout == SingleHostLayout) {
        return first;
    }
    if (hostLayout != DualHorizontalHostLayout || !first.isValid()) {
        return QSize();
    }

    const QSize second = virtualModeSize(virtualModes.value(1));
    if (!second.isValid()) {
        return QSize();
    }
    const int width = first.width() + second.width();
    if (width > MaximumVirtualCanvasWidth) {
        return QSize();
    }
    return QSize(width, qMax(first.height(), second.height()));
}

bool NvOutputTopology::fromJson(const QJsonObject& object,
                                NvOutputTopology& topology, QString* error)
{
    if (object.contains("capture") ||
            (object.value("feature_flags").toInt() & FixedCaptureFeature)) {
        const bool valid = parseFixedCapture(object, topology);
        if (!valid && error) *error = QStringLiteral("Unsupported or malformed fixed capture description");
        return valid;
    }
    NvOutputTopology parsed;
    if (!requireInteger(object, "schema_version", parsed.schemaVersion) ||
            parsed.schemaVersion != ProtocolVersion ||
            !requireInteger(object, "feature_flags", parsed.featureFlags) ||
            (parsed.featureFlags & (OutputTopologyFeature | SelectedOutputFeature |
                                    UnifiedAbsoluteInputFeature |
                                    HostLayoutMetadataFeature |
                                    CompositeSourceRegionsFeature |
                                    HostLayoutBindingFeature |
                                    IndependentVirtualModesFeature |
                                    DynamicHostLayoutFeature |
                                    TemporaryPhysicalLayoutFeature |
                                    FixedTransportMtuFeature |
                                    SessionTakeoverFeature)) !=
                (OutputTopologyFeature | SelectedOutputFeature |
                 UnifiedAbsoluteInputFeature |
                 HostLayoutMetadataFeature |
                 CompositeSourceRegionsFeature |
                 HostLayoutBindingFeature |
                 IndependentVirtualModesFeature |
                 DynamicHostLayoutFeature |
                 TemporaryPhysicalLayoutFeature |
                 FixedTransportMtuFeature |
                 SessionTakeoverFeature) ||
            !object.value("generation").isString() ||
            !object.value("layout").isObject() ||
            !object.value("desktop").isObject() ||
            !object.value("outputs").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unsupported or malformed output topology header");
        }
        return false;
    }
    parsed.generation = object.value("generation").toString();
    const QJsonObject layout = object.value("layout").toObject();
    int declaredOutputCount = 0;
    parsed.layoutKind = layout.value("kind").toString();
    parsed.startupLayoutKind = layout.value("startup_kind").toString();
    if (!layout.value("allowed_kinds").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host allowed-layout list");
        }
        return false;
    }
    for (const QJsonValue& kind : layout.value("allowed_kinds").toArray()) {
        if (!kind.isString() || !validLayoutKind(kind.toString()) ||
                parsed.allowedLayoutKinds.contains(kind.toString())) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid host allowed layout");
            }
            return false;
        }
        parsed.allowedLayoutKinds.append(kind.toString());
    }
    if (!layout.value("virtual_modes").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host virtual-mode list");
        }
        return false;
    }
    for (const QJsonValue& mode : layout.value("virtual_modes").toArray()) {
        if (!mode.isString() || !qualifiedVirtualModes().contains(mode.toString())) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid host virtual mode");
            }
            return false;
        }
        parsed.virtualModes.append(mode.toString());
    }
    if (!validLayoutKind(parsed.layoutKind) ||
            !validLayoutKind(parsed.startupLayoutKind) ||
            parsed.allowedLayoutKinds.isEmpty() ||
            !parsed.allowedLayoutKinds.contains(parsed.layoutKind) ||
            (parsed.startupLayoutKind == PhysicalHostLayout &&
             parsed.allowedLayoutKinds != QStringList {
                 QString::fromLatin1(PhysicalHostLayout),
                 QString::fromLatin1(SingleHostLayout),
                 QString::fromLatin1(DualHorizontalHostLayout)}) ||
            (parsed.startupLayoutKind != PhysicalHostLayout &&
             parsed.allowedLayoutKinds != QStringList {
                 QString::fromLatin1(SingleHostLayout),
                 QString::fromLatin1(DualHorizontalHostLayout)}) ||
            !layout.value("virtual").isBool() ||
            !requireInteger(layout, "output_count", declaredOutputCount) ||
            declaredOutputCount <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host display layout metadata");
        }
        return false;
    }
    parsed.virtualLayout = layout.value("virtual").toBool();
    if ((parsed.layoutKind == PhysicalHostLayout &&
         (parsed.virtualLayout || !parsed.virtualModes.isEmpty())) ||
            (parsed.layoutKind == SingleHostLayout &&
             (!parsed.virtualLayout || parsed.virtualModes.size() != 1)) ||
            (parsed.layoutKind == DualHorizontalHostLayout &&
             (!parsed.virtualLayout || parsed.virtualModes.size() != 2))) {
        if (error != nullptr) {
            *error = QStringLiteral("Inconsistent host display layout metadata");
        }
        return false;
    }
    const QJsonObject desktop = object.value("desktop").toObject();
    if (!requireInteger(desktop, "x", parsed.desktopX) ||
            !requireInteger(desktop, "y", parsed.desktopY) ||
            !requireInteger(desktop, "width", parsed.desktopWidth) ||
            !requireInteger(desktop, "height", parsed.desktopHeight) ||
            parsed.desktopWidth <= 0 || parsed.desktopHeight <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid output topology desktop bounds");
        }
        return false;
    }

    for (const QJsonValue& value : object.value("outputs").toArray()) {
        if (!value.isObject()) {
            return false;
        }
        const QJsonObject entry = value.toObject();
        NvOutput output;
        output.id = entry.value("id").toString();
        output.name = entry.value("name").toString();
        if (output.id.isEmpty() || output.name.isEmpty() ||
                !requireInteger(entry, "x", output.x) ||
                !requireInteger(entry, "y", output.y) ||
                !requireInteger(entry, "width", output.width) ||
                !requireInteger(entry, "height", output.height) ||
                !requireInteger(entry, "rotation", output.rotation) ||
                !requireInteger(entry, "refresh_millihz", output.refreshMillihz) ||
                !entry.value("primary").isBool() ||
                !entry.value("virtual").isBool() ||
                !entry.value("source_rect").isObject() || output.width <= 0 ||
                output.height <= 0 || parsed.contains(output.id)) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid or duplicate output entry");
            }
            return false;
        }
        output.primary = entry.value("primary").toBool();
        output.virtualOutput = entry.value("virtual").toBool();
        output.configuredMode = entry.value("configured_mode").toString();
        const QJsonObject sourceRect = entry.value("source_rect").toObject();
        if (!requireInteger(sourceRect, "x", output.sourceX) ||
                !requireInteger(sourceRect, "y", output.sourceY) ||
                !requireInteger(sourceRect, "width", output.sourceWidth) ||
                !requireInteger(sourceRect, "height", output.sourceHeight) ||
                output.sourceX < 0 || output.sourceY < 0 ||
                output.sourceWidth <= 0 || output.sourceHeight <= 0 ||
                output.sourceX + output.sourceWidth > parsed.desktopWidth ||
                output.sourceY + output.sourceHeight > parsed.desktopHeight ||
                output.virtualOutput != parsed.virtualLayout ||
                (parsed.virtualLayout &&
                 (parsed.outputs.size() >= parsed.virtualModes.size() ||
                  output.configuredMode != parsed.virtualModes[parsed.outputs.size()] ||
                  virtualModeSize(output.configuredMode) != QSize(output.width, output.height))) ||
                (!parsed.virtualLayout && !output.configuredMode.isEmpty())) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid composite source rectangle or output provenance");
            }
            return false;
        }
        parsed.outputs.append(output);
    }
    if (parsed.outputs.isEmpty() || parsed.outputs.size() != declaredOutputCount ||
            (parsed.layoutKind == SingleHostLayout && parsed.outputs.size() != 1) ||
            (parsed.layoutKind == DualHorizontalHostLayout && parsed.outputs.size() != 2)) {
        if (error != nullptr) {
            *error = QStringLiteral("Host reported no connected outputs");
        }
        return false;
    }
    topology = parsed;
    return true;
}

QJsonObject NvOutputTopology::toJson() const
{
    if (featureFlags == FixedCaptureFlags && outputs.size() == 1) {
        return {{"schema_version", schemaVersion}, {"feature_flags", featureFlags},
                {"generation", generation}, {"capture", QJsonObject {
                    {"id", outputs.first().id}, {"width", desktopWidth}, {"height", desktopHeight},
                    {"logical_bounds", QJsonObject {{"x", captureLogicalBounds.x()}, {"y", captureLogicalBounds.y()},
                        {"width", captureLogicalBounds.width()}, {"height", captureLogicalBounds.height()}}},
                    {"encoding_profile", applePreviewProfile(appleEncodingMode)}}}};
    }
    QJsonArray serializedOutputs;
    for (const NvOutput& output : outputs) {
        serializedOutputs.append(QJsonObject {
            {"id", output.id}, {"name", output.name},
            {"x", output.x}, {"y", output.y},
            {"width", output.width}, {"height", output.height},
            {"rotation", output.rotation},
            {"refresh_millihz", output.refreshMillihz},
            {"primary", output.primary},
            {"virtual", output.virtualOutput},
            {"configured_mode", output.configuredMode},
            {"source_rect", QJsonObject {
                {"x", output.sourceX}, {"y", output.sourceY},
                {"width", output.sourceWidth}, {"height", output.sourceHeight},
            }},
        });
    }
    return QJsonObject {
        {"schema_version", schemaVersion},
        {"feature_flags", featureFlags},
        {"generation", generation},
        {"layout", QJsonObject {
            {"kind", layoutKind}, {"virtual", virtualLayout},
            {"virtual_modes", QJsonArray::fromStringList(virtualModes)},
            {"output_count", outputs.size()},
            {"startup_kind", startupLayoutKind},
            {"allowed_kinds", QJsonArray::fromStringList(allowedLayoutKinds)},
        }},
        {"desktop", QJsonObject {
            {"x", desktopX}, {"y", desktopY},
            {"width", desktopWidth}, {"height", desktopHeight},
        }},
        {"outputs", serializedOutputs},
    };
}

bool NvOutputTopology::contains(QString outputId) const
{
    for (const NvOutput& output : outputs) {
        if (output.id == outputId) {
            return true;
        }
    }
    return false;
}

bool NvOutputTopology::displayPolicyKnown() const
{
    if (schemaVersion == ProtocolVersion && featureFlags == FixedCaptureFlags) return true;
    return schemaVersion == ProtocolVersion &&
            validLayoutKind(layoutKind) && validLayoutKind(startupLayoutKind) &&
            !allowedLayoutKinds.isEmpty();
}

bool NvOutputTopology::allowsBookmarkHostLayout(const QString& layout) const
{
    if (featureFlags == FixedCaptureFlags) return layout == QStringLiteral("fixed") || layout == MatchClientHostLayout;
    if (!displayPolicyKnown()) {
        return true;
    }
    if (layout == MatchClientHostLayout) {
        return allowedLayoutKinds.contains(SingleHostLayout) &&
                allowedLayoutKinds.contains(DualHorizontalHostLayout);
    }
    return allowedLayoutKinds.contains(layout);
}

int NvOutputTopology::outputCountForLayout(const QString& resolvedLayout) const
{
    if (resolvedLayout == SingleHostLayout) return 1;
    if (resolvedLayout == DualHorizontalHostLayout) return 2;
    if (resolvedLayout == PhysicalHostLayout || resolvedLayout == QStringLiteral("fixed"))
        return outputs.size();
    return 0;
}

bool NvOutputTopology::matchesRequestedHostLayout(const QString& layout,
                                                  const QStringList& modes) const
{
    if (layoutKind != layout || virtualModes != modes) {
        return false;
    }
    if (layout == PhysicalHostLayout) {
        return !virtualLayout && !outputs.isEmpty();
    }
    if (layout == SingleHostLayout) {
        return virtualLayout && outputs.size() == 1;
    }
    if (layout != DualHorizontalHostLayout || !virtualLayout || outputs.size() != 2) {
        return false;
    }

    const NvOutput& left = outputs.at(0);
    const NvOutput& right = outputs.at(1);
    return left.y == right.y &&
            right.x == left.x + left.width &&
            desktopX == left.x &&
            desktopY == left.y &&
            desktopWidth == left.width + right.width &&
            desktopHeight == qMax(left.height, right.height) &&
            left.sourceX == 0 && left.sourceY == 0 &&
            right.sourceX == left.width && right.sourceY == 0;
}

int NvOutputTopology::hostPlatform(int version, int flags)
{
    if (!supportsDescription(version, flags)) return 0;
    return flags == FixedCaptureFlags ? 2 : 1;
}

QSize NvOutputTopology::macDisplayModeSize(const QString& mode)
{
    const auto parts = mode.split(QLatin1Char('x'));
    if (parts.size() != 2) return {};
    bool widthOk, heightOk;
    const int width = parts[0].toInt(&widthOk), height = parts[1].toInt(&heightOk);
    if (!widthOk || !heightOk || width < 2 || height < 2 || width > 8192 || height > 8192 ||
            width % 2 || height % 2 || mode != QStringLiteral("%1x%2").arg(width).arg(height)) return {};
    return QSize(width, height);
}

QJsonObject NvOutputTopology::macDisplayRequest(const QString& mode, const QString& encodingMode, int scale)
{
    const QSize size = macDisplayModeSize(mode);
    if (!size.isValid() || (scale != 1 && scale != 2) ||
            (encodingMode != QLatin1String("hevc-10-420-videotoolbox") &&
             encodingMode != QLatin1String("hevc-10-444-videotoolbox"))) return {};
    return {{"schema_version", 3}, {"width", size.width()}, {"height", size.height()},
            {"scale", scale}, {"encoding_mode", encodingMode}};
}

QString NvOutputTopology::resolveMacClientDisplayMode(const QVector<NvClientDisplay>& displays, QString* error, int* scale)
{
    if (error) error->clear();
    if (scale) *scale = 1;
    if (displays.isEmpty() || displays.size() > 2) {
        if (error) *error = QStringLiteral("Match client displays requires exactly one or two active client monitors.");
        return {};
    }
    if (displays.size() == 2) {
        const QRect a = displays[0].bounds, b = displays[1].bounds;
        if (!(a.right() < b.left() || b.right() < a.left()) ||
                a.top() > b.bottom() || b.top() > a.bottom()) {
            if (error) *error = QStringLiteral("Match client displays currently requires two monitors arranged left to right.");
            return {};
        }
    }
    int width = 0, height = 0, canvasScale = 0;
    for (const auto& display : displays) {
        const QSize pixels = display.backingSize.isValid() ? display.backingSize : display.nativeSize;
        const int displayScale = display.backingSize.isValid() && pixels == display.bounds.size() * 2 ? 2 : 1;
        if ((display.backingSize.isValid() && pixels != display.bounds.size() * displayScale) ||
                (canvasScale && canvasScale != displayScale)) {
            if (error) *error = QStringLiteral("Match Client requires the same 1x or 2x Retina scale on both monitors. Use a manual resolution for mixed scaling.");
            return {};
        }
        canvasScale = displayScale;
        const QString size = QStringLiteral("%1x%2").arg(pixels.width()).arg(pixels.height());
        if (!display.bounds.isValid() || !macDisplayModeSize(size).isValid()) {
            if (error) *error = QStringLiteral("Mac desktop dimensions must be even pixel counts between 2 and 8192. Detected %1.").arg(size);
            return {};
        }
        width += pixels.width();
        height = qMax(height, pixels.height());
    }
    const QString mode = QStringLiteral("%1x%2").arg(width).arg(height);
    if (!macDisplayModeSize(mode).isValid()) {
        if (error) *error = QStringLiteral("The client display canvas (%1) exceeds the Mac desktop size limit.").arg(mode);
        return {};
    }
    if (scale) *scale = canvasScale;
    return mode;
}

bool NvOutputTopology::resolveClientDisplayLayout(QVector<NvClientDisplay> displays,
                                                  QString& hostLayout,
                                                  QStringList& virtualModes,
                                                  QString* error,
                                                  int* primaryOutput)
{
    hostLayout.clear();
    virtualModes.clear();
    if (displays.size() < 1 || displays.size() > 2) {
        if (error != nullptr) {
            *error = QStringLiteral("Match client displays requires exactly one or two active client monitors.");
        }
        return false;
    }

    std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
        return std::make_tuple(left.bounds.x(), left.bounds.y()) <
                std::make_tuple(right.bounds.x(), right.bounds.y());
    });
    if (displays.size() == 2) {
        const QRect& left = displays.at(0).bounds;
        const QRect& right = displays.at(1).bounds;
        const bool horizontallySeparated = left.right() < right.left();
        const bool verticallyOverlapping =
                left.top() <= right.bottom() && right.top() <= left.bottom();
        if (!horizontallySeparated || !verticallyOverlapping) {
            if (error != nullptr) {
                *error = QStringLiteral("Match client displays currently requires two monitors arranged left to right.");
            }
            return false;
        }
    }

    for (const NvClientDisplay& display : displays) {
        const QString mode = QStringLiteral("%1x%2")
                .arg(display.nativeSize.width()).arg(display.nativeSize.height());
        if (!qualifiedVirtualModes().contains(mode)) {
            if (error != nullptr) {
                *error = QStringLiteral("Client monitor resolution %1 is not a qualified PLANK virtual mode.")
                        .arg(mode);
            }
            hostLayout.clear();
            virtualModes.clear();
            return false;
        }
        virtualModes.append(mode);
    }
    hostLayout = displays.size() == 1 ? QString::fromLatin1(SingleHostLayout) :
                                       QString::fromLatin1(DualHorizontalHostLayout);
    if (primaryOutput) {
        *primaryOutput = clientPrimaryIndex(displays);
        if (*primaryOutput < 0) {
            if (error) *error = QStringLiteral("Unable to identify one primary client display. Please reconnect.");
            hostLayout.clear();
            virtualModes.clear();
            return false;
        }
    }
    return true;
}

int NvOutputTopology::clientPrimaryIndex(QVector<NvClientDisplay> displays)
{
    std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
        return std::make_tuple(left.bounds.x(), left.bounds.y()) <
                std::make_tuple(right.bounds.x(), right.bounds.y());
    });
    int primary = -1;
    for (int index = 0; index < displays.size(); ++index) {
        if (!displays[index].primary) continue;
        if (primary != -1) return -1;
        primary = index;
    }
    return primary;
}
