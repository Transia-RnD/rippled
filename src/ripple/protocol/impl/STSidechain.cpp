//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2022 Ripple Labs Inc.

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

#include <ripple/protocol/STSidechain.h>

#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/jss.h>
#include "ripple/protocol/SField.h"
#include "ripple/protocol/tokens.h"

namespace ripple {

STSidechain::STSidechain(
    AccountID const& srcChainDoor,
    Issue const& srcChainIssue,
    AccountID const& dstChainDoor,
    Issue const& dstChainIssue,
    SField const& f)
    : STBase(f)
    , srcChainDoor_{srcChainDoor}
    , srcChainIssue_{srcChainIssue}
    , dstChainDoor_{dstChainDoor}
    , dstChainIssue_{dstChainIssue}
{
}

STSidechain::STSidechain(SField const& f) : STBase(f)
{
}

STSidechain::value_type const&
STSidechain::value() const noexcept
{
    return *this;
}

STSidechain::STSidechain(SerialIter& sit, SField const& name) : STBase(name)
{
    srcChainDoor_ = sit.get160();
    srcChainIssue_.currency = sit.get160();
    srcChainIssue_.account = sit.get160();

    dstChainDoor_ = sit.get160();
    dstChainIssue_.currency = sit.get160();
    dstChainIssue_.account = sit.get160();
}

STBase*
STSidechain::copy(std::size_t n, void* buf) const
{
    return emplace(n, buf, *this);
}

STBase*
STSidechain::move(std::size_t n, void* buf)
{
    return emplace(n, buf, std::move(*this));
}

SerializedTypeID
STSidechain::getSType() const
{
    return STI_SIDECHAIN;
}
void
STSidechain::add(Serializer& s) const
{
    s.addBitString(srcChainDoor_);
    s.addBitString(srcChainIssue_.currency);
    s.addBitString(srcChainIssue_.account);

    s.addBitString(dstChainDoor_);
    s.addBitString(dstChainIssue_.currency);
    s.addBitString(dstChainIssue_.account);
}

bool
STSidechain::isEquivalent(const STBase& t) const
{
    const STSidechain* v = dynamic_cast<const STSidechain*>(&t);
    return v && (*v == *this);
}

bool
STSidechain::isDefault() const
{
    return srcChainDoor_.isZero() && srcChainIssue_.currency.isZero() &&
        srcChainIssue_.account.isZero() && dstChainDoor_.isZero() &&
        dstChainIssue_.currency.isZero() && dstChainIssue_.account.isZero();
}

STSidechain
STSidechainFromJson(SField const& name, Json::Value const& v)
{
    if (!v.isObject())
    {
        Throw<std::runtime_error>(
            "STSidechain can only be specified with a 'object' Json value");
    }

    Json::Value const srcChainDoorStr = v[jss::src_chain_door];
    Json::Value const srcChainIssue = v[jss::src_chain_issue];
    Json::Value const dstChainDoorStr = v[jss::dst_chain_door];
    Json::Value const dstChainIssue = v[jss::dst_chain_issue];

    if (!srcChainDoorStr.isString())
    {
        Throw<std::runtime_error>(
            "STSidechain src_chain_door must be a string Json value");
    }
    if (!dstChainDoorStr.isString())
    {
        Throw<std::runtime_error>(
            "STSidechain dst_chain_door must be a string Json value");
    }

    auto const srcChainDoor =
        parseBase58<AccountID>(srcChainDoorStr.asString());
    auto const dstChainDoor =
        parseBase58<AccountID>(dstChainDoorStr.asString());
    if (!srcChainDoor)
    {
        Throw<std::runtime_error>(
            "STSidechain src_chain_door must be a valid account");
    }
    if (!dstChainDoor)
    {
        Throw<std::runtime_error>(
            "STSidechain dst_chain_door must be a valid account");
    }

    return STSidechain{
        *srcChainDoor,
        issueFromJson(srcChainIssue),
        *dstChainDoor,
        issueFromJson(dstChainIssue),
        name};
}

}  // namespace ripple
