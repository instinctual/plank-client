import Foundation
import Security

// Private to each signed app/device. No shared access group or existing raw
// Relay/Setup approvals. Only explicit transcript verification creates a pin.
enum PlankPencilRelayKeys {
    static let service = "la.instinctual.plank.pencil-relay.v1"
    static func read(_ account: String) throws -> Data? {
        let q: [String:Any] = [kSecClass as String:kSecClassGenericPassword,
            kSecAttrService as String:service,kSecAttrAccount as String:account,
            kSecReturnData as String:true,kSecMatchLimit as String:kSecMatchLimitOne]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary,&item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw PlankPencilWireError.storage }
        return data
    }
    static func write(_ data: Data, account: String) throws {
        let q: [String:Any] = [kSecClass as String:kSecClassGenericPassword,
            kSecAttrService as String:service,kSecAttrAccount as String:account]
        let attrs: [String:Any] = [kSecValueData as String:data,
            kSecAttrAccessible as String:kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        var status = SecItemAdd(q.merging(attrs) { _,new in new } as CFDictionary,nil)
        if status == errSecDuplicateItem { status = SecItemUpdate(q as CFDictionary,[kSecValueData as String:data] as CFDictionary) }
        guard status == errSecSuccess else { throw PlankPencilWireError.storage }
    }
    static func privateKey() throws -> Data {
        if let key = try read("private") {
            guard key.count == 32 else { throw PlankPencilWireError.storage }; return key
        }
        var key = [UInt8](repeating:0,count:32)
        guard SecRandomCopyBytes(kSecRandomDefault,32,&key) == errSecSuccess else { throw PlankPencilWireError.crypto }
        let data = Data(key); try write(data,account:"private"); return data
    }
    static func publicKey(_ privateKey: Data) throws -> Data {
        guard privateKey.count == 32 else { throw PlankPencilWireError.invalid }
        var key = [UInt8](repeating:0,count:32)
        let result = privateKey.withUnsafeBytes { plank_pencil_crypto_public_key($0.bindMemory(to:UInt8.self).baseAddress,&key) }
        guard result == 0 else { throw PlankPencilWireError.crypto }; return Data(key)
    }
    static func hex(_ key: Data) -> String { key.map { String(format:"%02x",$0) }.joined() }
    static func unhex(_ text: String) -> Data? {
        guard text.count == 64 else { return nil }
        var bytes: [UInt8] = []; var i = text.startIndex
        while i < text.endIndex {
            let end = text.index(i,offsetBy:2)
            guard let byte = UInt8(text[i..<end],radix:16) else { return nil }
            bytes.append(byte); i = end
        }
        guard bytes.contains(where: { $0 != 0 }) else { return nil }; return Data(bytes)
    }
    // Keep the existing identity and v1/v2 approval records intact. Generic key
    // batches require a separate v3 comparison, never a broadened old pin.
    static func approvalAccount(_ key: Data) -> String { "peer-v3-" + hex(key) }
    static func approved(_ key: Data) throws -> Bool { try read(approvalAccount(key)) == key }
    static func approve(_ key: Data) throws {
        guard key.count == 32 else { throw PlankPencilWireError.invalid }
        try write(key,account:approvalAccount(key))
    }
}
