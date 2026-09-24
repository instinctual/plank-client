#pragma once

#include "outputtopology.h"
#include "macmediafeatures.h"
#include <Limelight.h>
#include <QByteArray>

// Explicit adapters for schemas4/5/6 and independently versioned schema7
// features. Negotiation never changes the selected video encoding profile.
namespace MacPreviewLaunch {
#if defined(Q_OS_LINUX)
constexpr bool CameraSupported = true;
#else
constexpr bool CameraSupported = false;
#endif

inline QJsonObject request(const NvOutputTopology& topology, int bitrateKbps,
                           int udpPayloadSize, const MacMediaFeatures::Agreement& agreement)
{
    NvOutputTopology checked;
    if (agreement.launchSchema < 4 || agreement.launchSchema > 7 ||
            agreement.encodingMode != topology.appleEncodingMode ||
            topology.featureFlags != NvOutputTopology::FixedCaptureFlags ||
            !NvOutputTopology::fromJson(topology.toJson(), checked) ||
            bitrateKbps < 10000 || bitrateKbps > 150000 ||
            udpPayloadSize < 1200 || udpPayloadSize > 65527) return {};
    QJsonObject result {{"schema_version", agreement.launchSchema}, {"capture_generation", checked.generation},
            {"capture_id", checked.outputs.first().id},
            {"width", checked.desktopWidth}, {"height", checked.desktopHeight},
            {"encoding_mode", checked.appleEncodingMode}, {"frame_rate", 60},
            {"bitrate_kbps", bitrateKbps}, {"max_udp_payload_size", udpPayloadSize},
            {"clipboard", agreement.enabled("clipboard")}, {"microphone", agreement.enabled("microphone")}};
    if (agreement.launchSchema == 6) result.insert("camera", agreement.enabled("camera"));
    if (agreement.launchSchema == 7) {
        auto features = agreement.features;
        auto desktop = features.value("desktop").toObject();
        for (const char* key : {"width", "height", "encoding_mode", "frame_rate", "bitrate_kbps"}) {
            desktop.insert(QLatin1String(key), result.take(QLatin1String(key)));
        }
        features.insert("desktop", desktop);
        result.remove("clipboard"); result.remove("microphone");
        result.insert("transport", MacMediaFeatures::transport());
        result.insert("required_features", agreement.requiredFeatures);
        result.insert("features", features);
    }
    return result;
}
inline QJsonObject request(const NvOutputTopology& topology, int bitrateKbps, int udpPayloadSize)
{
    return request(topology, bitrateKbps, udpPayloadSize, MacMediaFeatures::legacy(6, topology.appleEncodingMode));
}

struct Reply {
    QByteArray transportToken;
    PLANK_NATIVE_SESSION_CONFIGURATION configuration {};
    bool clipboard = false;
    bool microphone = false;
    unsigned microphoneSchema = 2;
    bool camera = false;
};

inline bool parseReply(const QJsonObject& object, const NvOutputTopology& topology,
                       int approvedControlPort, int udpPayloadSize, Reply& reply,
                       const MacMediaFeatures::Agreement& agreement, int bitrateKbps)
{
    // Always clear an earlier successful result before parsing a new response.
    reply.transportToken.fill('\0');
    reply = {};
    const auto sent = request(topology, bitrateKbps, udpPayloadSize, agreement);
    if (sent.isEmpty() ||
            approvedControlPort < 1 || approvedControlPort > 65535 ||
            (agreement.launchSchema == 7 ? object.size() > 16 : object.size() != 7) ||
            object.value("schema_version") != QJsonValue(agreement.launchSchema) ||
            object.value("state") != QJsonValue("connecting") ||
            object.value("udp_port") != QJsonValue(approvedControlPort) ||
            object.value("max_udp_payload_size") != QJsonValue(udpPayloadSize) ||
            object.value("capture") != topology.toJson().value("capture")) return false;
    bool clipboard = false, microphone = false, camera = false;
    if (agreement.launchSchema == 7) {
        if (object.value("transport") != MacMediaFeatures::transport() ||
            !MacMediaFeatures::validRequired(object.value("required_features")) ||
            !object.value("features").isObject() || object.value("features").toObject().size() > 32) return false;
        const auto features = object.value("features").toObject();
        const auto expected = sent.value("features").toObject();
        for (const auto& name : agreement.requiredFeatures)
            if (!object.value("required_features").toArray().contains(name)) return false;
        for (const auto& name : MacMediaFeatures::names()) {
            const auto value = features.value(name);
            if ((value.isNull() || value.isUndefined()) && !object.value("required_features").toArray().contains(name)) continue;
            if (!agreement.enabled(name) || !MacMediaFeatures::containsProfile(value, expected.value(name).toObject())) return false;
        }
        clipboard = features.value("clipboard").isObject();
        microphone = features.value("microphone").isObject();
        camera = features.value("camera").isObject();
    } else {
        const auto services = object.value("services").toObject();
        if (!services.value("clipboard").isBool() || !services.value("microphone").isBool()) return false;
        clipboard = services.value("clipboard").toBool();
        microphone = services.value("microphone").toBool();
        QJsonObject expected {{"audio", true}, {"input", true}, {"pen", "normalized"}, {"cursor", "embedded"},
            {"clipboard", clipboard}, {"microphone", microphone}};
        if (agreement.launchSchema >= 6) {
            if (!services.value("camera").isBool()) return false;
            camera = services.value("camera").toBool(); expected.insert("camera", camera);
        }
        if (services != expected || (clipboard && !agreement.enabled("clipboard")) ||
            (microphone && !agreement.enabled("microphone")) || (camera && !agreement.enabled("camera"))) return false;
    }

    const QString token = object.value("transport_token").toString();
    if (token.size() != 44) return false;
    const QByteArray encoded = token.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != 32 || decoded.decoded.toBase64() != encoded) return false;
    reply.transportToken = encoded;
    reply.clipboard = clipboard;
    reply.microphone = microphone;
    if (microphone && agreement.launchSchema == 7)
        reply.microphoneSchema = unsigned(object.value("features").toObject().value("microphone").toObject().value("schema_version").toInt());
    reply.camera = camera;
    reply.configuration.structSize = sizeof(reply.configuration);
    reply.configuration.negotiatedVideoFormat = topology.appleEncodingMode == QLatin1String("hevc-10-444-videotoolbox") ?
                VIDEO_FORMAT_H265_REXT10_444 : VIDEO_FORMAT_H265_MAIN10;
    // Schema 2 explicitly supports normalized pen and PLD1 bitrate updates.
    reply.configuration.hostFeatureFlags = LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK | LI_FF_PEN_TOUCH_EVENTS;
    reply.configuration.sessionPort = static_cast<uint32_t>(approvedControlPort);
    reply.configuration.serviceFlags = PLANK_NATIVE_SERVICE_AUDIO | PLANK_NATIVE_SERVICE_INPUT;
    reply.configuration.audioPacketDurationMs = 5;
    reply.configuration.opusConfiguration.sampleRate = 48000;
    reply.configuration.opusConfiguration.channelCount = 2;
    reply.configuration.opusConfiguration.streams = 1;
    reply.configuration.opusConfiguration.coupledStreams = 1;
    reply.configuration.opusConfiguration.mapping[0] = 0;
    reply.configuration.opusConfiguration.mapping[1] = 1;
    return true;
}

inline bool parseReply(const QJsonObject& object, const NvOutputTopology& topology,
                       int approvedControlPort, int udpPayloadSize, Reply& reply)
{
    return parseReply(object, topology, approvedControlPort, udpPayloadSize, reply,
        MacMediaFeatures::legacy(6, topology.appleEncodingMode), 10000);
}

} // namespace MacPreviewLaunch
