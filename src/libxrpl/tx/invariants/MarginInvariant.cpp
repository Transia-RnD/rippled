#include <xrpl/tx/invariants/MarginInvariant.h>
//
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STNumber.h>

namespace xrpl {

void
ValidMarginAccount::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    auto const check = [&](std::shared_ptr<SLE const> const& sle) {
        if (!sle)
            return;

        switch (sle->getType())
        {
            case ltMARGIN_ACCOUNT: {
                Number const collateral =
                    sle->at(~sfCollateralBalance).value_or(Number(0));
                if (collateral < Number(0))
                    negativeCollateral_ = true;
                break;
            }
            case ltMARGIN_POSITION: {
                Number const allocated =
                    sle->at(~sfAllocatedMargin).value_or(Number(0));
                if (allocated < Number(0))
                    negativeAllocatedMargin_ = true;
                break;
            }
            case ltINSURANCE_VAULT: {
                Number const balance =
                    sle->at(~sfInsuranceBalance).value_or(Number(0));
                if (balance < Number(0))
                    negativeInsuranceBalance_ = true;
                break;
            }
            default:
                break;
        }
    };

    // Check the after state (or before if deleting)
    check(after);

    // Special check: deleted margin position should have zero allocated margin
    if (isDelete && before && before->getType() == ltMARGIN_POSITION)
    {
        Number const allocated =
            before->at(~sfAllocatedMargin).value_or(Number(0));
        if (allocated > Number(0))
            deletedWithMargin_ = true;
    }
}

bool
ValidMarginAccount::finalize(
    STTx const& tx,
    TER const result,
    XRPAmount const,
    ReadView const& view,
    beast::Journal const& j)
{
    if (result != tesSUCCESS)
        return true;

    if (!view.rules().enabled(featureOptions))
        return true;

    if (negativeCollateral_)
    {
        JLOG(j.fatal())
            << "Invariant failed: margin account has negative collateral balance.";
        return false;
    }

    if (negativeAllocatedMargin_)
    {
        JLOG(j.fatal())
            << "Invariant failed: margin position has negative allocated margin.";
        return false;
    }

    if (negativeInsuranceBalance_)
    {
        JLOG(j.fatal())
            << "Invariant failed: insurance vault has negative balance.";
        return false;
    }

    if (deletedWithMargin_)
    {
        JLOG(j.fatal())
            << "Invariant failed: deleted margin position had non-zero allocated margin.";
        return false;
    }

    return true;
}

}  // namespace xrpl
