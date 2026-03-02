#pragma once

#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/tx/Transactor.h>

#include <cstdint>
#include <vector>

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
