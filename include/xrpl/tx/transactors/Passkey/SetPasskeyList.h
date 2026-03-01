#pragma once

#include <xrpl/tx/Transactor.h>

namespace xrpl {

class SetPasskeyList : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Blocker};

    explicit SetPasskeyList(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    TER
    doApply() override;
};

using PasskeyListSet = SetPasskeyList;

}  // namespace xrpl
