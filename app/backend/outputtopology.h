#pragma once

#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

struct NvOutput
{
    QString id;
    QString name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;
    int refreshMillihz = 0;
    bool primary = false;
    bool virtualOutput = false;
    QString configuredMode;
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
};

struct NvClientDisplay
{
    QRect bounds;
    QSize nativeSize;
    QSize backingSize {}; // macOS current compositor pixels; absent on other platforms
    bool primary = false;
};

struct NvOutputTopology
{
    static const int ProtocolVersion = 13;
    static const int OutputTopologyFeature = 0x1;
    static const int SelectedOutputFeature = 0x2;
    static const int UnifiedAbsoluteInputFeature = 0x4;
    static const int ScaledSpanFeature = 0x8;
    static const int TopologyGenerationFeature = 0x10;
    static const int HostLayoutMetadataFeature = 0x20;
    static const int CompositeSourceRegionsFeature = 0x40;
    static const int HostLayoutBindingFeature = 0x80;
    static const int IndependentVirtualModesFeature = 0x100;
    static const int DynamicHostLayoutFeature = 0x200;
    static const int TemporaryPhysicalLayoutFeature = 0x400;
    static const int CaptureSourceSelectionFeature = 0x800;
    static const int EncoderBackendSelectionFeature = 0x1000;
    static const int NvfbcHevc10NvencFeature = 0x2000;
    static const int FixedTransportMtuFeature = 0x4000;
    static const int SessionTakeoverFeature = 0x8000;
    static const int DesktopHandoffNoticeFeature = 0x10000;
    static const int AuthenticatedDesktopStageFeature = 0x20000;
    static const int WorkerInstanceFeature = 0x40000;
    static const int VirtualPrimaryConnectorFeature = 0x2000000;
    // Fixed capture description only. Not part of the Linux launch feature
    // mask: parsing this does not grant input, layout changes or media launch.
    static const int FixedCaptureFeature = 0x80000;
    static const int MacDesktopPreparationFeature = 0x100000;
    static const int MacEncodingProfileFeature = 0x200000;
    static const int ClipboardSyncFeature = 0x400000;
    // Only advertise a clipboard receiver/sender when this client implements
    // it. Linux must not cause the host to read or transmit unused clipboard data.
#ifdef Q_OS_MACOS
    static const int PlatformClipboardSyncFeature = ClipboardSyncFeature;
#else
    static const int PlatformClipboardSyncFeature = 0;
#endif
    static const int FixedCaptureFlags = FixedCaptureFeature | OutputTopologyFeature |
            TopologyGenerationFeature | HostLayoutMetadataFeature | CompositeSourceRegionsFeature |
            MacDesktopPreparationFeature | MacEncodingProfileFeature | ClipboardSyncFeature;
    static const int MaximumVirtualCanvasWidth = 8192;
    static const int SupportedFeatureFlags = OutputTopologyFeature |
                                             SelectedOutputFeature |
                                             UnifiedAbsoluteInputFeature |
                                             ScaledSpanFeature |
                                             TopologyGenerationFeature |
                                             HostLayoutMetadataFeature |
                                             CompositeSourceRegionsFeature |
                                             HostLayoutBindingFeature |
                                             IndependentVirtualModesFeature |
                                             DynamicHostLayoutFeature |
                                             TemporaryPhysicalLayoutFeature |
                                             CaptureSourceSelectionFeature |
                                             EncoderBackendSelectionFeature |
                                             NvfbcHevc10NvencFeature |
                                             FixedTransportMtuFeature |
                                             SessionTakeoverFeature |
                                             DesktopHandoffNoticeFeature |
                                             AuthenticatedDesktopStageFeature |
                                             WorkerInstanceFeature |
                                             VirtualPrimaryConnectorFeature |
                                             PlatformClipboardSyncFeature;
    static const char* NativeScalingMode;
    static const char* ScaledSpanMode;
    static const char* MatchClientHostLayout;
    static const char* PhysicalHostLayout;
    static const char* SingleHostLayout;
    static const char* DualHorizontalHostLayout;

    // Discovery decides whether to fetch topology; the authenticated JSON
    // parser still validates the complete platform-specific contract.
    static bool supportsDescription(int version, int featureFlags);
    // UI hint only: unknown=0, Linux=1, macOS=2. Never authorization.
    static int hostPlatform(int version, int featureFlags);
    static bool fromJson(const QJsonObject& object, NvOutputTopology& topology,
                         QString* error = nullptr);
    QJsonObject toJson() const;

    static bool resolveClientDisplayLayout(QVector<NvClientDisplay> displays,
                                           QString& hostLayout,
                                           QStringList& virtualModes,
                                           QString* error = nullptr,
                                           int* primaryOutput = nullptr);
    static int clientPrimaryIndex(QVector<NvClientDisplay> displays);
    static QStringList qualifiedVirtualModes();
    static QString resolveMacClientDisplayMode(const QVector<NvClientDisplay>& displays,
                                               QString* error = nullptr, int* scale = nullptr);
    static QJsonObject macDisplayRequest(const QString& mode, const QString& encodingMode, int scale);
    static QSize virtualModeSize(const QString& mode);
    static QSize macDisplayModeSize(const QString& mode);
    static QSize virtualCanvasSize(const QString& hostLayout,
                                   const QStringList& virtualModes);
    bool displayPolicyKnown() const;
    bool allowsBookmarkHostLayout(const QString& layout) const;
    // Requested layouts may still be transitioning from the authenticated one.
    int outputCountForLayout(const QString& resolvedLayout) const;
    bool matchesRequestedHostLayout(const QString& layout,
                                    const QStringList& modes) const;
    bool contains(QString outputId) const;

    int schemaVersion = 0;
    int featureFlags = 0;
    QString generation;
    int desktopX = 0;
    int desktopY = 0;
    int desktopWidth = 0;
    int desktopHeight = 0;
    QString layoutKind;
    QString startupLayoutKind;
    QStringList allowedLayoutKinds;
    bool virtualLayout = false;
    QStringList virtualModes;
    QVector<NvOutput> outputs;
    QRectF captureLogicalBounds;
    QString appleEncodingMode = QStringLiteral("hevc-10-420-videotoolbox");
};
