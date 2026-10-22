import Foundation

@main enum ControlContactChecks {
    static func main() {
        var contacts = PlankIPadControlContactPolicy<Int>()
        let shift = PlankControlBinding(code:0x10)
        let space = PlankControlBinding(code:0x20)
        precondition(contacts.proposal(id:1,source:.pencil,inside:true,enabled:true,binding:shift,behavior:.hold) == nil)
        precondition(contacts.proposal(id:1,source:.indirect,inside:true,enabled:true,binding:shift,behavior:.hold) == nil)
        precondition(contacts.proposal(id:1,source:.direct,inside:false,enabled:true,binding:shift,behavior:.hold) == nil)
        precondition(contacts.proposal(id:1,source:.direct,inside:true,enabled:false,binding:shift,behavior:.hold) == nil)
        let finger = contacts.proposal(id:1,source:.direct,inside:true,enabled:true,binding:shift,behavior:.hold)!
        precondition(contacts.accept(finger) && contacts.hasHeldContact)
        precondition(contacts.proposal(id:1,source:.direct,inside:true,enabled:true,binding:space,behavior:.hold) == nil)
        // Pencil and indirect input do not add or replace shortcut ownership.
        precondition(contacts.proposal(id:2,source:.pencil,inside:true,enabled:true,binding:space,behavior:.hold) == nil)
        precondition(contacts.movement(id:1)?.owner == finger.owner)
        precondition(contacts.movement(id:1)?.binding == shift && contacts.activeCount == 1)
        let second = contacts.proposal(id:2,source:.direct,inside:true,enabled:true,binding:space,behavior:.hold)!
        precondition(contacts.accept(second) && contacts.activeCount == 2)
        precondition(contacts.finish(id:2)?.owner == second.owner && contacts.hasHeldContact)
        precondition(contacts.finish(id:2) == nil, "Repeated end/cancel must not release another owner")
        let pending = contacts.proposal(id:3,source:.direct,inside:true,enabled:true,binding:space,behavior:.tap)!
        let retired = contacts.retire()
        precondition(retired.count == 1 && retired[0].owner == finger.owner && contacts.isEmpty)
        precondition(!contacts.accept(pending), "A proposal cannot survive disable/layout/geometry retirement")
        precondition(contacts.finish(id:1) == nil, "Late genuine cancellation after retirement is inert")
        for index in 0..<PlankIPadControlContactPolicy<Int>.maximumContacts {
            let proposal = contacts.proposal(id:index,source:.direct,inside:true,enabled:true,binding:shift,behavior:.hold)!
            precondition(contacts.accept(proposal))
        }
        precondition(contacts.proposal(id:99,source:.direct,inside:true,enabled:true,binding:shift,behavior:.hold) == nil)
        precondition(contacts.retire().count == PlankIPadControlContactPolicy<Int>.maximumContacts)
        print("PASS control contacts: direct fingers only, bounded independent owners, stable Pencil/hover/movement ownership, exact end/cancel, stale epoch rejection and teardown")
    }
}
