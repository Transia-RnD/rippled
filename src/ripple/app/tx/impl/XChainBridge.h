//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_TX_XCHAINBRIDGE_H_INCLUDED
#define RIPPLE_TX_XCHAINBRIDGE_H_INCLUDED

#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/protocol/SField.h>
#include "ripple/protocol/STXChainAttestationBatch.h"

namespace ripple {

namespace AttestationBatch {
struct AttestationClaim;
}

class BridgeCreate : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit BridgeCreate(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

class BridgeModify : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit BridgeModify(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};
//------------------------------------------------------------------------------

class XChainClaim : public Transactor
{
public:
    // TODO: "Normal" isn't right - as rewards are paid, but we don't know the
    // reward amount on preflight
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit XChainClaim(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

//------------------------------------------------------------------------------

class XChainCommit : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Custom};

    static TxConsequences
    makeTxConsequences(PreflightContext const& ctx);

    explicit XChainCommit(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

//------------------------------------------------------------------------------

class XChainCreateClaimID : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit XChainCreateClaimID(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

//------------------------------------------------------------------------------

class XChainAddAttestation : public Transactor
{
public:
    // TODO: "Normal" isn't right - as rewards are paid, but we don't know if
    // the account submitting this transaction will pay some of the rewards or
    // not
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit XChainAddAttestation(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;

private:
    // Apply the attestations for a claim id
    // signersList is a map from a signer's account id to its weight
    TER
    applyClaims(
        STXChainAttestationBatch::TClaims::const_iterator attBegin,
        STXChainAttestationBatch::TClaims::const_iterator attEnd,
        STXChainBridge const& bridgeSpec,
        STXChainBridge::ChainType const srcChain,
        std::unordered_map<AccountID, std::uint32_t> const& signersList,
        std::uint32_t quorum);

    // Apply an attestation for an account create
    // signersList is a map from a signer's account id to its weight
    TER
    applyCreateAccountAtt(
        STXChainAttestationBatch::TCreates::const_iterator attBegin,
        STXChainAttestationBatch::TCreates::const_iterator attEnd,
        AccountID const& doorAccount,
        Keylet const& doorK,
        STXChainBridge const& bridgeSpec,
        Keylet const& bridgeK,
        STXChainBridge::ChainType const srcChain,
        std::unordered_map<AccountID, std::uint32_t> const& signersList,
        std::uint32_t quorum);
};

//------------------------------------------------------------------------------

class XChainCreateAccount : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit XChainCreateAccount(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

//------------------------------------------------------------------------------

}  // namespace ripple

#endif
