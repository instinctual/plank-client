import Foundation

/// Finger ownership is independent of Pencil contacts and hover. In particular,
/// moving just beyond a key does not release a modifier that is still held.
struct PlankIPadControlContactPolicy<ID: Hashable> {
    enum Source { case direct, pencil, indirect }
    struct Contact {
        let id: ID
        let owner: UUID
        let binding: PlankControlBinding
        let behavior: PlankControlBehavior
        fileprivate let epoch: UInt64
    }
    static var maximumContacts: Int { 16 }
    private var contacts: [ID: Contact] = [:]
    private var epoch: UInt64 = 0
    var activeCount: Int { contacts.count }
    var hasHeldContact: Bool { contacts.values.contains { $0.behavior == .hold } }
    var isEmpty: Bool { contacts.isEmpty }

    /// The caller offers a held binding before committing this proposal. No
    /// transport callback runs while the value is under mutable access.
    func proposal(id: ID, source: Source, inside: Bool, enabled: Bool,
                  binding: PlankControlBinding, behavior: PlankControlBehavior) -> Contact? {
        guard source == .direct, inside, enabled, binding.isValid,
              contacts[id] == nil, contacts.count < Self.maximumContacts else { return nil }
        return Contact(id:id,owner:UUID(),binding:binding,behavior:behavior,epoch:epoch)
    }
    mutating func accept(_ contact: Contact) -> Bool {
        guard contact.epoch == epoch, contacts[contact.id] == nil,
              contacts.count < Self.maximumContacts else { return false }
        contacts[contact.id] = contact
        return true
    }
    /// UIKit retains the original target through movement. A held key survives
    /// finger jitter, a new Pencil contact, and Pencil hover until real lift or
    /// cancellation. A tap still tests its final position in the view adapter.
    func movement(id: ID) -> Contact? { contacts[id] }
    mutating func finish(id: ID) -> Contact? { contacts.removeValue(forKey:id) }
    mutating func retire() -> [Contact] {
        let retired = Array(contacts.values)
        contacts.removeAll()
        epoch &+= 1
        return retired
    }
}
