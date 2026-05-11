#pragma once

#include <xrpld/app/inner/detail/InnerAccountHelpers.h>
#include <xrpld/app/inner/detail/InnerTransactor.h>

namespace xrpl {
namespace inner {

class InnerWithdraw : public InnerTransactor
{
public:
    using InnerTransactor::InnerTransactor;

    TER
    preflight() const override
    {
        if (tx().amount == 0)
            return temBAD_AMOUNT;
        if (tx().source == AccountID{})
            return temBAD_SRC_ACCOUNT;
        return tesSUCCESS;
    }

    TER
    preclaim() const override
    {
        auto const src = readAccount(
            const_cast<InnerView&>(ctx_.view), tx().source);
        if (!src)
            return terNO_ACCOUNT;
        if (src->balance < tx().amount)
            return tecUNFUNDED;
        return tesSUCCESS;
    }

    TER
    doApply() override
    {
        auto acct = readAccount(view(), tx().source);
        if (!acct)
            return tefINTERNAL;

        if (acct->balance < tx().amount)
            return tecUNFUNDED;

        acct->balance -= tx().amount;
        acct->sequence++;
        writeAccount(view(), *acct, false);

        return tesSUCCESS;
    }
};

}  // namespace inner
}  // namespace xrpl
