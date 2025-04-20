//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2025 Ripple Labs Inc.

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

#ifndef RIPPLE_TX_IMPL_DETAILS_OPTIONUTILS_H_INCLUDED
#define RIPPLE_TX_IMPL_DETAILS_OPTIONUTILS_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/Sandbox.h>

namespace ripple {

namespace option {

struct SealedOptionData
{
    uint256 offerID;
    AccountID account;
    std::uint32_t quantitySealed;
    STAmount premium;
};

std::vector<SealedOptionData>
matchOptions(
    Sandbox& sb,
    Issue issue,
    std::uint64_t strike,
    std::uint32_t expiration,
    bool isPut,
    bool isSell,
    std::uint32_t desiredQuantity,
    AccountID const& account,
    uint256 const& optionIndex,
    bool isMarketOrder = true,
    STAmount const& limitPrice = STAmount(0));

TER
createOffer(
    Sandbox& sb,
    AccountID const& account,
    Keylet const& optionOfferKeylet,
    std::uint32_t flags,
    std::uint32_t quantity,
    std::uint32_t openInterest,
    STAmount const& premium,
    bool isSell,
    STAmount const& quantityShares,
    Issue const& issue,
    STAmount strikePrice,
    std::int64_t strike,
    std::uint32_t expiration,
    Keylet const& optionBookDirKeylet,
    std::vector<SealedOptionData> const& sealedOptions,
    beast::Journal j_);

TER
lockTokens(
    Sandbox& sb,
    XRPAmount const& sourceBalance,
    AccountID const& account,
    STAmount const& quantityShares,
    beast::Journal j_);

TER
unlockTokens(
    Sandbox& sb,
    AccountID const& receiver,
    std::shared_ptr<SLE> const& sleReceiver,
    STAmount const& quantityShares,
    beast::Journal j_);

TER
transferTokens(
    Sandbox& sb,
    AccountID const& sender,
    AccountID const& receiver,
    STAmount const& amount,
    beast::Journal j_);

TER
closeOffer(
    Sandbox& sb,
    AccountID const& account,
    Keylet const& offerKeylet,
    bool isPut,
    bool isSell,
    Issue const& issue,
    std::uint64_t const& strike,
    std::uint32_t const& expiration,
    beast::Journal j);

TER
exerciseOffer(
    Sandbox& sb,
    bool isPut,
    STAmount const& strikePrice,
    AccountID const& buyer,
    std::shared_ptr<SLE> const& sleBuyer,
    Issue const& issue,
    STArray const& sealedOptions,
    beast::Journal j_);

TER
expireOffer(ApplyView& view, std::shared_ptr<SLE> const& sle, beast::Journal j);

TER
deleteOffer(ApplyView& view, std::shared_ptr<SLE> const& sle, beast::Journal j);

}  // namespace option
}  // namespace ripple

#endif  // RIPPLE_TX_IMPL_DETAILS_OPTIONUTILS_H_INCLUDED
