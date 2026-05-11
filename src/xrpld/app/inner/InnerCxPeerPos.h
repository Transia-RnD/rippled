#pragma once

#include <xrpld/app/inner/InnerCxLedger.h>
#include <xrpld/app/inner/InnerCxTx.h>
#include <xrpld/consensus/ConsensusProposal.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/PublicKey.h>

#include <boost/container/static_vector.hpp>

#include <cstdint>
#include <string>

namespace xrpl {
namespace inner {

class InnerCxPeerPos
{
public:
    using Proposal = ConsensusProposal<NodeID, uint256, uint256>;

    InnerCxPeerPos(
        PublicKey const& publicKey,
        Slice const& signature,
        uint256 const& suppress,
        Proposal const& proposal)
        : publicKey_(publicKey)
        , suppression_(suppress)
        , proposal_(proposal)
    {
        auto const sigData = signature;
        signature_.assign(sigData.begin(), sigData.end());
    }

    bool
    checkSign() const
    {
        return verifyDigest(
            publicKey_, proposal_.signingHash(), signature(), false);
    }

    Slice
    signature() const
    {
        return {signature_.data(), signature_.size()};
    }

    PublicKey const&
    publicKey() const
    {
        return publicKey_;
    }

    uint256 const&
    suppressionID() const
    {
        return suppression_;
    }

    Proposal const&
    proposal() const
    {
        return proposal_;
    }

    json::Value
    getJson() const
    {
        json::Value j;
        j["node"] = to_string(proposal_.nodeID());
        j["seq"] = proposal_.proposeSeq();
        return j;
    }

    std::string
    render() const
    {
        return to_string(proposal_.prevLedger());
    }

private:
    PublicKey publicKey_;
    boost::container::static_vector<std::uint8_t, 72> signature_;
    uint256 suppression_;
    Proposal proposal_;
};

}  // namespace inner
}  // namespace xrpl
