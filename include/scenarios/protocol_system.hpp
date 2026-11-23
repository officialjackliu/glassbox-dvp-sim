#pragma once

// Wires the protocol nodes into a Simulator: one Coordinator and three
// Participants (BANK_A, BANK_B, BANK_C), configured from the simulator's
// timer settings. Returns non-owning observers for assertions and state
// views; the Simulator owns the nodes. Nodes never see each other or the
// simulator: all interaction is through messages.

#include "sim/simulator.hpp"
#include "domain/local_ledger.hpp"
#include "protocol/coordinator.hpp"
#include "protocol/participant.hpp"

#include <array>

namespace glassbox {

struct BankGenesis {
    LocalLedger bank_a;
    LocalLedger bank_b;
    LocalLedger bank_c;
};

struct ProtocolNodes {
    Coordinator* coordinator = nullptr;
    Participant* bank_a = nullptr;
    Participant* bank_b = nullptr;
    Participant* bank_c = nullptr;

    // Returns nullptr for the coordinator id.
    [[nodiscard]] Participant* bank(NodeId node) const noexcept;
    [[nodiscard]] std::array<Participant*, 3> banks() const noexcept;
};

[[nodiscard]] ProtocolNodes install_protocol_nodes(
    Simulator& simulator,
    const BankGenesis& genesis);

} // namespace glassbox
