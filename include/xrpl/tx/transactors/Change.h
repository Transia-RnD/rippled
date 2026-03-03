#pragma once

#include <xrpl/tx/Transactor.h>

namespace xrpl {

class Change : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit Change(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    TER
    doApply() override;
    void
    preCompute() override;

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx)
    {
        return XRPAmount{0};
    }

    static TER
    preclaim(PreclaimContext const& ctx);

private:
    TER
    applyAmendment();

    TER
    applyFee();

    TER
    applyUNLModify();

    TER
    applyUNLReport();

    TER
    applyImportCredit();

    TER
    applyExportConfirm();
};

using EnableAmendment = Change;
using SetFee = Change;
using UNLModify = Change;
using UNLReportTx = Change;
using ImportCreditTx = Change;
using ExportConfirmTx = Change;

}  // namespace xrpl
