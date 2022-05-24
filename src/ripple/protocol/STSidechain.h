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

#ifndef RIPPLE_PROTOCOL_STSIDECHAIN_H_INCLUDED
#define RIPPLE_PROTOCOL_STSIDECHAIN_H_INCLUDED

#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/STBase.h>

namespace ripple {

// variable length byte string
class STSidechain final : public STBase
{
    AccountID srcChainDoor_{};
    Issue srcChainIssue_{};
    AccountID dstChainDoor_{};
    Issue dstChainIssue_{};

public:
    using value_type = STSidechain;

    STSidechain() = default;
    STSidechain(STSidechain const& rhs) = default;
    STSidechain&
    operator=(STSidechain const& rhs) = default;

    STSidechain(
        AccountID const& srcChainDoor,
        Issue const& srcChainIssue,
        AccountID const& dstChainDoor,
        Issue const& dstChainIssue,
        SField const& f = sfGeneric);

    STSidechain(SField const& f);

    STSidechain(SerialIter&, SField const& name = sfGeneric);

    SerializedTypeID
    getSType() const override;

    void
    add(Serializer& s) const override;

    bool
    isEquivalent(const STBase& t) const override;

    bool
    isDefault() const override;

    STSidechain::value_type const&
    value() const noexcept;

    friend bool
    operator==(STSidechain const& lhs, STSidechain const& rhs)
    {
        return std::tie(
                   lhs.srcChainDoor_,
                   lhs.srcChainIssue_,
                   lhs.dstChainDoor_,
                   lhs.dstChainIssue_) ==
            std::tie(
                   rhs.srcChainDoor_,
                   rhs.srcChainIssue_,
                   rhs.dstChainDoor_,
                   rhs.dstChainIssue_);
    }

    friend bool
    operator!=(STSidechain const& lhs, STSidechain const& rhs)
    {
        return !(lhs == rhs);
    }

    AccountID const&
    srcChainDoor() const
    {
        return srcChainDoor_;
    };
    Issue const&
    srcChainIssue() const
    {
        return srcChainIssue_;
    };
    AccountID const&
    dstChainDoor() const
    {
        return dstChainDoor_;
    };
    Issue const&
    dstChainIssue() const
    {
        return dstChainIssue_;
    };

private:
    STBase*
    copy(std::size_t n, void* buf) const override;
    STBase*
    move(std::size_t n, void* buf) override;

    friend class detail::STVar;
};

STSidechain
STSidechainFromJson(SField const& name, Json::Value const& v);

}  // namespace ripple

#endif
