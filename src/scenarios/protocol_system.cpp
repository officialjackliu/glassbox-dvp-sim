// Creates the coordinator and the three banks and installs them in a
// Simulator.

#include "scenarios/protocol_system.hpp"

#include <memory>

namespace glassbox {

Participant* ProtocolNodes::bank(NodeId node) const noexcept
{
    switch (node) {
    case NodeId::bank_a:
        return bank_a;
    case NodeId::bank_b:
        return bank_b;
    case NodeId::bank_c:
        return bank_c;
    case NodeId::coordinator:
        break;
    }
    return nullptr;
}

std::array<Participant*, 3> ProtocolNodes::banks() const noexcept
{
    return {bank_a, bank_b, bank_c};
}

ProtocolNodes install_protocol_nodes(Simulator& simulator,
                                     const BankGenesis& genesis)
{
    const TimerConfig& timers = simulator.config().timers;
    ProtocolNodes nodes;

    auto coordinator = std::make_unique<Coordinator>(
        CoordinatorConfig{timers.vote_timeout_us, timers.retransmit});
    nodes.coordinator = coordinator.get();
    simulator.set_node(NodeId::coordinator, std::move(coordinator));

    const auto install_bank = [&](NodeId id, const LocalLedger& ledger) {
        auto participant = std::make_unique<Participant>(
            ParticipantConfig{id, ledger, timers.decision_wait});
        Participant* observer = participant.get();
        simulator.set_node(id, std::move(participant));
        return observer;
    };
    nodes.bank_a = install_bank(NodeId::bank_a, genesis.bank_a);
    nodes.bank_b = install_bank(NodeId::bank_b, genesis.bank_b);
    nodes.bank_c = install_bank(NodeId::bank_c, genesis.bank_c);
    return nodes;
}

} // namespace glassbox
