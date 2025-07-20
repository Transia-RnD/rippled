#ifndef RIPPLE_TX_SETQUANTUMKEY_H_INCLUDED
#define RIPPLE_TX_SETQUANTUMKEY_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>

namespace ripple {

class SetQuantumKey : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit SetQuantumKey(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

} // namespace ripple

#endif