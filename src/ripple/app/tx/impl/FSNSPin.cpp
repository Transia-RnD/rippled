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

#include <ripple/app/tx/impl/FSNSPin.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/digest.h>

namespace ripple {

XRPAmount
FSNSPin::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

NotTEC
FSNSPin::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    std::uint32_t const txFlags = ctx.tx.getFlags();

    if (ctx.tx.isFieldPresent(sfData))
    {
        auto const data = ctx.tx.getFieldVL(sfData);

        if (data.size() < 1 || data.size() > 512)
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction. Data must be at least 1 "
                    "character and no more than 512 characters.";
            return temMALFORMED;
        }
    }

    return preflight2(ctx);
}

TER
FSNSPin::preclaim(PreclaimContext const& ctx)
{
    if (ctx.tx.isFieldPresent(sfData))
    {
        auto const data = ctx.tx.getFieldVL(sfData);
        ripple::uint256 dataHash = ripple::sha512Half_s(
            ripple::Slice(data.data(), data.size())
        );

        if (ctx.view.exists(keylet::fs(dataHash)))
            return tecDUPLICATE;
    }

    return tesSUCCESS;
}

TER
FSNSPin::doApply()
{
    auto const sle = view().peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    auto const nickname = ctx_.tx.getFieldH256(sfNickname);
    ripple::uint256 rootIndex;
    if (ctx_.tx.isFieldPresent(sfRootIndex))
    {
        rootIndex = ctx_.tx.getFieldH256(sfRootIndex);
    }
    else if (ctx_.tx.isFieldPresent(sfData))
    {
        auto const data = ctx_.tx.getFieldVL(sfData);
        auto const dataHash = ripple::sha512Half_s(
            ripple::Slice(data.data(), data.size())
        );

        auto const fsKeylet = keylet::fs(dataHash);
        rootIndex = fsKeylet.key;
        auto const sleFS = std::make_shared<SLE>(fsKeylet);

        sleFS->setFieldVL(sfData, data);
        (*sleFS)[sfOwner] = account_;

        view().insert(sleFS);
    }
    else
    {
        // pass
    }

    auto const fsnsKeylet = keylet::fsns(nickname);
    auto const nsSle = ctx_.view().peek(fsnsKeylet);
    if (!nsSle)
    {
        auto const sleFSNS = std::make_shared<SLE>(fsnsKeylet);
        sleFSNS->setFieldH256(sfRootIndex, rootIndex);
        (*sleFSNS)[sfOwner] = account_;
        view().insert(sleFSNS);
    }
    else
    {
        // if (account_ != *nsSle[sfOwner])
        // {
        //     return tefINTERNAL;
        // }
        (*nsSle)[sfRootIndex] = rootIndex;
        view().update(nsSle);
    }
    
    return tesSUCCESS;
}

}  // namespace ripple
