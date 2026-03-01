#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/tx/Transactor.h>

namespace xrpl {

class Import : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Custom};

    explicit Import(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    /** Computes the starting bonus for newly created accounts.
        Returns ReserveBase + ReserveIncrement * 5, or 2 XRP fallback. */
    template <typename V>
    static XRPAmount
    computeStartingBonus(V const& v)
    {
        auto const& fees = v.read(keylet::fees());

        uint64_t b = 1'000'000;
        uint64_t i = 200'000;

        // new fee object format
        if (fees && fees->isFieldPresent(sfReserveBaseDrops) &&
            fees->isFieldPresent(sfReserveIncrementDrops))
        {
            auto const base = fees->getFieldAmount(sfReserveBaseDrops);
            auto const incr = fees->getFieldAmount(sfReserveIncrementDrops);

            if (isXRP(base) && isXRP(incr))
            {
                b = base.xrp().drops();
                i = incr.xrp().drops();
            }
        }

        // old object format
        if (fees && fees->isFieldPresent(sfReserveBase) &&
            fees->isFieldPresent(sfReserveIncrement))
        {
            b = fees->getFieldU32(sfReserveBase);
            i = fees->getFieldU32(sfReserveIncrement);
        }

        uint64_t x = b + i * 5U;
        if (x > i && x > b)
            return XRPAmount{static_cast<XRPAmount::value_type>(x)};

        // fallback in case of overflow
        return XRPAmount{2 * DROPS_PER_XRP};
    }

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    static TxConsequences
    makeTxConsequences(PreflightContext const& ctx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace xrpl
