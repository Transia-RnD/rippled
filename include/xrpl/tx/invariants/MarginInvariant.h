#pragma once

#include <xrpl/basics/Number.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

#include <vector>

namespace xrpl {

/**
 * @brief Invariants: Margin accounts and positions have valid state
 *
 * - CollateralBalance is never negative
 * - AllocatedMargin on a position is never negative
 * - A deleted margin position must have zero allocated margin
 * - InsuranceBalance is never negative
 */
class ValidMarginAccount
{
    bool bad_ = false;
    bool negativeCollateral_ = false;
    bool negativeAllocatedMargin_ = false;
    bool negativeInsuranceBalance_ = false;
    bool deletedWithMargin_ = false;

public:
    void
    visitEntry(bool, std::shared_ptr<SLE const> const&, std::shared_ptr<SLE const> const&);

    bool
    finalize(STTx const&, TER const, XRPAmount const, ReadView const&, beast::Journal const&);
};

}  // namespace xrpl
