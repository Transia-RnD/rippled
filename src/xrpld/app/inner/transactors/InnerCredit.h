#pragma once

#include <xrpld/app/inner/detail/InnerAccountHelpers.h>
#include <xrpld/app/inner/detail/InnerTransactor.h>

namespace xrpl {
namespace inner {

class InnerCredit : public InnerTransactor
{
public:
    using InnerTransactor::InnerTransactor;

    TER
    preflight() const override
    {
        if (tx().amount == 0)
            return temBAD_AMOUNT;
        if (tx().destination == AccountID{})
            return temDST_NEEDED;
        return tesSUCCESS;
    }

    TER
    doApply() override
    {
        auto const& dest = tx().destination;
        auto existing = readAccount(view(), dest);

        InnerAccountFields acct;
        bool isNew = false;

        if (existing)
        {
            acct = *existing;
        }
        else
        {
            acct.id = dest;
            acct.balance = 0;
            acct.sequence = 0;
            isNew = true;
        }

        acct.balance += tx().amount;
        writeAccount(view(), acct, isNew);

        return tesSUCCESS;
    }
};

}  // namespace inner
}  // namespace xrpl
