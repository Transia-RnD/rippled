#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/tx/Transactor.h>

namespace xrpl {

class Export : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Blocker};

    explicit Export(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace xrpl
