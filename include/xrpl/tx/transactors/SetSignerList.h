#pragma once

#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/tx/SignerEntries.h>
#include <xrpl/tx/Transactor.h>

#include <cstdint>
#include <vector>

namespace xrpl {

/**
See the README.md for an overview of the SetSignerList transaction that
this class implements.
*/
class SetSignerList : public Transactor
{
private:
    // Values determined during preCompute for use later.
    enum Operation { unknown, set, destroy };
    Operation do_{unknown};
    std::uint32_t quorum_{0};
    std::vector<SignerEntries::SignerEntry> signers_;

public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Blocker};

    explicit SetSignerList(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static std::uint32_t
    getFlagsMask(PreflightContext const& ctx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    TER
    doApply() override;
    void
    preCompute() override;

    // Interface used by DeleteAccount
    static TER
    removeFromLedger(
        ServiceRegistry& registry,
        ApplyView& view,
        AccountID const& account,
        beast::Journal j);

    // Public static methods used by Import for cross-chain key imports
    static NotTEC
    validateQuorumAndSignerEntries(
        std::uint32_t quorum,
        std::vector<SignerEntries::SignerEntry> const& signers,
        AccountID const& account,
        beast::Journal j,
        Rules const&);

    static TER
    replaceSignersFromLedger(
        ServiceRegistry& registry,
        ApplyView& view,
        beast::Journal j,
        AccountID const& acc,
        std::uint32_t quorum,
        std::vector<SignerEntries::SignerEntry> const& signers,
        XRPAmount const mPriorBalance);

private:
    static std::tuple<NotTEC, std::uint32_t, std::vector<SignerEntries::SignerEntry>, Operation>
    determineOperation(STTx const& tx, ApplyFlags flags, beast::Journal j);

    TER
    replaceSignerList();
    TER
    destroySignerList();

    void
    writeSignersToSLE(SLE::pointer const& ledgerEntry, std::uint32_t flags) const;

    static void
    writeSignersToSLE(
        ApplyView& view,
        std::shared_ptr<SLE> const& ledgerEntry,
        std::uint32_t flags,
        std::uint32_t quorum,
        std::vector<SignerEntries::SignerEntry> const& signers);
};

using SignerListSet = SetSignerList;

}  // namespace xrpl
