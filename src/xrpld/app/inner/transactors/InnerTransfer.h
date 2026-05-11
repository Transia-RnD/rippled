#pragma once

#include <xrpld/app/inner/detail/InnerAccountHelpers.h>
#include <xrpld/app/inner/detail/InnerTransactor.h>

namespace xrpl {
namespace inner {

class InnerTransfer : public InnerTransactor
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
        if (tx().destination == AccountID{})
            return temDST_NEEDED;
        if (tx().source == tx().destination)
            return temDST_IS_SRC;
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
        auto srcAcct = readAccount(view(), tx().source);
        if (!srcAcct)
            return tefINTERNAL;

        if (srcAcct->balance < tx().amount)
            return tecUNFUNDED;

        srcAcct->balance -= tx().amount;
        srcAcct->sequence++;
        writeAccount(view(), *srcAcct, false);

        auto dstAcct = readAccount(view(), tx().destination);
        bool isNew = false;
        if (!dstAcct)
        {
            dstAcct = InnerAccountFields{};
            dstAcct->id = tx().destination;
            dstAcct->balance = 0;
            dstAcct->sequence = 0;
            isNew = true;
        }

        dstAcct->balance += tx().amount;
        writeAccount(view(), *dstAcct, isNew);

        return tesSUCCESS;
    }
};

}  // namespace inner
}  // namespace xrpl
