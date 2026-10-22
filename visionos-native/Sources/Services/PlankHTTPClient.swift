import Foundation
import Security

enum PlankHTTPError: LocalizedError, Sendable {
    case invalidAddress
    case invalidCertificate
    case invalidResponse(String)
    case rejected(Int, String)

    var errorDescription: String? {
        switch self {
        case .invalidAddress:
            return "The workstation address is invalid."
        case .invalidCertificate:
            return "The workstation did not present a valid PLANK certificate."
        case let .invalidResponse(message):
            return message
        case let .rejected(code, message):
            return message.isEmpty ? "The Host rejected the request (\(code))." : message
        }
    }
}

private final class PlankTrustDelegate: NSObject, URLSessionDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var pinnedCertificate: Data?

    func urlSession(
        _ session: URLSession,
        didReceive challenge: URLAuthenticationChallenge,
        completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
    ) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust,
              Self.isQualifiedPlankTrust(trust),
              let certificate = SecTrustCopyCertificateChain(trust) as? [SecCertificate],
              let leaf = certificate.first else {
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }

        let certificateData = SecCertificateCopyData(leaf) as Data
        lock.lock()
        let accepted: Bool
        if let pinnedCertificate {
            accepted = pinnedCertificate == certificateData
        } else {
            pinnedCertificate = certificateData
            accepted = true
        }
        lock.unlock()

        guard accepted else {
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }

        completionHandler(.useCredential, URLCredential(trust: trust))
    }

    private static func isQualifiedPlankTrust(_ trust: SecTrust) -> Bool {
        guard let certificates = SecTrustCopyCertificateChain(trust) as? [SecCertificate],
              let certificate = certificates.first,
              let key = SecCertificateCopyKey(certificate),
              let attributes = SecKeyCopyAttributes(key) as? [CFString: Any],
              attributes[kSecAttrKeyType] as? String == kSecAttrKeyTypeRSA as String,
              let keySize = attributes[kSecAttrKeySizeInBits] as? Int,
              keySize >= 3072 else {
            return false
        }

        SecTrustSetPolicies(trust, SecPolicyCreateBasicX509())
        SecTrustSetAnchorCertificates(trust, [certificate] as CFArray)
        SecTrustSetAnchorCertificatesOnly(trust, true)

        var error: CFError?
        return SecTrustEvaluateWithError(trust, &error) &&
            SecTrustGetCertificateCount(trust) == 1
    }
}

private final class AppListParser: NSObject, XMLParserDelegate {
    private(set) var applications: [PlankApplication] = []
    private var title: String?
    private var identifier: Int?
    private var currentElement: String?
    private var currentText = ""

    func parser(_ parser: XMLParser, didStartElement elementName: String,
                namespaceURI: String?, qualifiedName qName: String?,
                attributes attributeDict: [String: String] = [:]) {
        if elementName == "App" {
            title = nil
            identifier = nil
        }
        currentElement = elementName
        currentText = ""
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        currentText += string
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String,
                namespaceURI: String?, qualifiedName qName: String?) {
        let value = currentText.trimmingCharacters(in: .whitespacesAndNewlines)
        if elementName == "AppTitle" { title = value }
        if elementName == "ID" { identifier = Int(value) }
        if elementName == "App", let title, let identifier {
            applications.append(PlankApplication(id: identifier, title: title))
        }
        currentElement = nil
        currentText = ""
    }
}

private final class ServerInfoParser: NSObject, XMLParserDelegate {
    private(set) var values: [String: String] = [:]
    private(set) var statusCode: Int?
    private(set) var statusMessage = ""

    private var currentElement: String?
    private var currentText = ""

    func parser(
        _ parser: XMLParser,
        didStartElement elementName: String,
        namespaceURI: String?,
        qualifiedName qName: String?,
        attributes attributeDict: [String: String] = [:]
    ) {
        if elementName == "root" {
            statusCode = Int(attributeDict["status_code"] ?? "")
            statusMessage = attributeDict["status_message"] ?? ""
        }
        currentElement = elementName
        currentText = ""
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        currentText += string
    }

    func parser(
        _ parser: XMLParser,
        didEndElement elementName: String,
        namespaceURI: String?,
        qualifiedName qName: String?
    ) {
        if currentElement == elementName {
            values[elementName] = currentText.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        currentElement = nil
        currentText = ""
    }
}

final class PlankHTTPClient: @unchecked Sendable {
    private let baseURL: URL
    private let session: URLSession

    private(set) var identity: PlankHostIdentity?
    private(set) var sessionToken: String?

    init(host: HostBookmark) throws {
        var components = URLComponents()
        components.scheme = "https"
        components.host = host.address.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        components.port = Int(host.port)
        guard let url = components.url else {
            throw PlankHTTPError.invalidAddress
        }
        baseURL = url

        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 5
        configuration.timeoutIntervalForResource = 10
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        configuration.urlCache = nil
        configuration.httpShouldSetCookies = false
        configuration.httpAdditionalHeaders = ["Connection": "close"]
        configuration.tlsMinimumSupportedProtocolVersion = .TLSv13
        configuration.tlsMaximumSupportedProtocolVersion = .TLSv13
        session = URLSession(
            configuration: configuration,
            delegate: PlankTrustDelegate(),
            delegateQueue: nil
        )
    }

    deinit {
        session.invalidateAndCancel()
    }

    func fetchServerInfo() async throws -> PlankHostIdentity {
        let data = try await request(path: "serverinfo")
        let parserDelegate = ServerInfoParser()
        let parser = XMLParser(data: data)
        parser.delegate = parserDelegate
        guard parser.parse(), let statusCode = parserDelegate.statusCode else {
            throw PlankHTTPError.invalidResponse("The Host returned malformed server information.")
        }
        guard statusCode == 200 else {
            throw PlankHTTPError.rejected(statusCode, parserDelegate.statusMessage)
        }

        let values = parserDelegate.values
        let hostIdentity = PlankHostIdentity(
            name: values["hostname"].flatMap { $0.isEmpty ? nil : $0 } ?? "PLANK Host",
            uniqueID: values["uniqueid"] ?? "",
            version: values["PlankHostVersion"] ?? values["appversion"] ?? "",
            supportsAuthentication: values["PlankAuth"] == "1"
        )
        identity = hostIdentity
        return hostIdentity
    }

    func authenticate(username: String, password: String) async throws -> PlankAuthentication {
        guard !username.isEmpty, sessionToken == nil else {
            throw PlankHTTPError.invalidResponse("The authentication request is no longer valid.")
        }

        var response = try await postAuthentication(
            path: "start",
            body: ["username": username]
        )

        for _ in 0..<16 {
            guard let state = response["state"] as? String else {
                throw PlankHTTPError.invalidResponse("The Host returned an invalid authentication response.")
            }

            switch state {
            case "authenticated":
                guard let token = response["session_token"] as? String, !token.isEmpty else {
                    throw PlankHTTPError.invalidResponse("Authentication returned no session token.")
                }
                sessionToken = token
                return PlankAuthentication(
                    sessionToken: token,
                    desktopStage: response["desktop_stage"] as? String ?? ""
                )
            case "denied":
                throw PlankHTTPError.rejected(401, "The username or password was not accepted.")
            case "busy":
                throw PlankHTTPError.rejected(503, "The Host is busy. Please try again shortly.")
            case "challenge":
                guard let conversationID = response["conversation_id"] as? String,
                      let messages = response["messages"] as? [[String: Any]] else {
                    throw PlankHTTPError.invalidResponse("The Host returned an invalid sign-in challenge.")
                }
                let answers = try messages.map { message -> String in
                    switch message["style"] as? Int {
                    case 1: return password
                    case 2: return username
                    case 3, 4: return ""
                    default:
                        throw PlankHTTPError.invalidResponse("The Host requested an unsupported sign-in prompt.")
                    }
                }
                response = try await postAuthentication(
                    path: "respond",
                    body: ["conversation_id": conversationID, "responses": answers]
                )
            default:
                throw PlankHTTPError.invalidResponse("The Host returned an unknown authentication state.")
            }
        }

        throw PlankHTTPError.invalidResponse("The Host sign-in exceeded its challenge limit.")
    }

    // Keep the URLSession's certificate pin when a replacement display worker
    // invalidates its in-memory bearer token. A fresh HTTP client would trust a
    // new certificate before resending credentials.
    func refreshAuthentication(username: String, password: String) async throws -> PlankAuthentication {
        sessionToken = nil
        return try await authenticate(username: username, password: password)
    }

    func fetchTopology() async throws -> PlankTopology {
        let data = try await authorizedRequest(path: "plank/topology")
        return try PlankTopologyDecoder.decode(data)
    }

    func fetchApplications() async throws -> [PlankApplication] {
        let data = try await authorizedRequest(path: "applist")
        let delegate = AppListParser()
        let parser = XMLParser(data: data)
        parser.delegate = delegate
        guard parser.parse(), !delegate.applications.isEmpty else {
            throw PlankHTTPError.invalidResponse("The Host returned no launchable applications.")
        }
        return delegate.applications
    }

    func launchDesktop(
        topology: PlankTopology,
        applicationID: Int,
        frameRate: Int,
        playAudioOnHost: Bool
    ) async throws -> PlankLaunchCredentials {
        let encodingMode = "hevc-10-444-nvenc"
        let udpPayloadMTU: UInt32 = 1200
        var query: [URLQueryItem] = [
            .init(name: "appid", value: String(applicationID)),
            .init(name: "mode", value: "\(topology.desktopWidth)x\(topology.desktopHeight)x\(frameRate)"),
            .init(name: "additionalStates", value: "1"),
            .init(name: "hdrMode", value: "1"),
            .init(name: "clientHdrCapVersion", value: "0"),
            .init(name: "clientHdrCapSupportedFlagsInUint32", value: "0"),
            .init(name: "clientHdrCapMetaDataId", value: "1"),
            .init(name: "clientHdrCapDisplayData", value: "0x0x0x0x0x0x0x0x0x0x0"),
            .init(name: "localAudioPlayMode",
                  value: PlankAudioPreferences.localAudioPlayMode(playOnHost: playAudioOnHost)),
            .init(name: "surroundAudioInfo", value: "196610"),
            .init(name: "remoteControllersBitmap", value: "0"),
            .init(name: "gcmap", value: "0"),
            .init(name: "gcpersist", value: "0"),
            .init(name: "plankProtocolVersion", value: String(topology.schemaVersion)),
            .init(name: "plankFeatureFlags", value: String(topology.featureFlags)),
            .init(name: "plankDisplayMode", value: topology.displayMode),
            .init(name: "plankCaptureSource", value: "nvfbc"),
            .init(name: "plankEncoderBackend", value: "nvenc-direct"),
            .init(name: "plankEncodingMode", value: encodingMode),
            .init(name: "plankQuicUdpPayloadMtu", value: String(udpPayloadMTU)),
            .init(name: "plankHostLayout", value: topology.layout.kind),
            .init(name: "plankTopologyGeneration", value: topology.generation),
        ]
        query.append(contentsOf: topology.launchLayoutQuery)

        let data = try await authorizedRequest(path: "launch", query: query, timeout: 120)
        let parserDelegate = ServerInfoParser()
        let parser = XMLParser(data: data)
        parser.delegate = parserDelegate
        guard parser.parse(), parserDelegate.statusCode == 200 else {
            throw PlankHTTPError.rejected(
                parserDelegate.statusCode ?? 500,
                parserDelegate.statusMessage
            )
        }
        let values = parserDelegate.values
        guard let portValue = UInt16(values["PlankTransportPort"] ?? ""), portValue > 0,
              let certificate = values["PlankTransportCertificateSha256"], certificate.count == 64,
              let token = values["PlankTransportToken"], token.count == 64,
              values["PlankEncodingMode"] == encodingMode,
              UInt32(values["PlankQuicUdpPayloadMtu"] ?? "") == udpPayloadMTU else {
            throw PlankHTTPError.invalidResponse("The Host returned invalid streaming credentials.")
        }
        return PlankLaunchCredentials(
            transportPort: portValue,
            certificateSHA256: certificate,
            transportToken: token,
            udpPayloadMTU: udpPayloadMTU,
            encodingMode: encodingMode
        )
    }

    private func request(path: String) async throws -> Data {
        let url = baseURL.appending(path: path)
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        let (data, response) = try await session.data(for: request)
        try validateHTTP(response)
        return data
    }

    private func authorizedRequest(
        path: String,
        query: [URLQueryItem] = [],
        timeout: TimeInterval? = nil
    ) async throws -> Data {
        guard let sessionToken, !sessionToken.isEmpty else {
            throw PlankHTTPError.invalidResponse("The workstation session is not authenticated.")
        }
        guard var components = URLComponents(
            url: baseURL.appending(path: path),
            resolvingAgainstBaseURL: false
        ) else {
            throw PlankHTTPError.invalidAddress
        }
        if !query.isEmpty { components.queryItems = query }
        guard let url = components.url else { throw PlankHTTPError.invalidAddress }
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.setValue("Bearer \(sessionToken)", forHTTPHeaderField: "Authorization")
        if let timeout { request.timeoutInterval = timeout }
        let (data, response) = try await session.data(for: request)
        try validateHTTP(response)
        // The Host uses a GameStream XML status envelope (with HTTP 200) when
        // an in-memory bearer token is invalidated by a display-worker change.
        // Surface its protocol status instead of feeding XML to a JSON parser.
        if data.first(where: { ![9, 10, 13, 32].contains($0) }) == 60 {
            let status = ServerInfoParser()
            let parser = XMLParser(data: data)
            parser.delegate = status
            if parser.parse(), let code = status.statusCode, code != 200 {
                throw PlankHTTPError.rejected(code, status.statusMessage)
            }
        }
        return data
    }

    private func postAuthentication(path: String, body: [String: Any]) async throws -> [String: Any] {
        let url = baseURL.appending(path: "plank/auth/\(path)")
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)

        let (data, response) = try await session.data(for: request)
        try validateHTTP(response)
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw PlankHTTPError.invalidResponse("The Host returned malformed authentication data.")
        }
        return object
    }

    private func validateHTTP(_ response: URLResponse) throws {
        guard let response = response as? HTTPURLResponse else {
            throw PlankHTTPError.invalidResponse("The Host returned no HTTP response.")
        }
        guard (200..<300).contains(response.statusCode) else {
            throw PlankHTTPError.rejected(response.statusCode, "The Host rejected the request.")
        }
    }
}
