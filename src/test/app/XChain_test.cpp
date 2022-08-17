//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2022 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/beast/unit_test/suite.hpp>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/XChainAttestations.h>

#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>
#include <test/jtx/multisign.h>
#include <test/jtx/xchain_bridge.h>

#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ripple {
namespace test {

template <class T>
struct xEnv : public jtx::XChainBridgeObjects
{
    jtx::Env env_;

    xEnv(T& s, bool side = false)
        : env_(s, jtx::envconfig(jtx::port_increment, side ? 3 : 0), features)
    {
        using namespace jtx;
        PrettyAmount xrp_funds{XRP(10000)};

        if (!side)
        {
            env_.fund(xrp_funds, mcDoor, mcAlice, mcBob, mcGw);

            // Signer's list must match the attestation signers
            env_(jtx::signers(mcDoor, signers.size(), signers));
        }
        else
        {
            env_.fund(
                xrp_funds, scDoor, scAlice, scBob, scGw, scAttester, scReward);

            // Signer's list must match the attestation signers
            env_(jtx::signers(scDoor, signers.size(), signers));
        }
    }

    xEnv&
    close()
    {
        env_.close();
        return *this;
    }

    template <class Arg, class... Args>
    xEnv&
    fund(STAmount const& amount, Arg const& arg, Args const&... args)
    {
        env_.fund(amount, arg, args...);
        return *this;
    }

    template <class JsonValue, class... FN>
    xEnv&
    tx(JsonValue&& jv, FN const&... fN)
    {
        env_(std::forward<JsonValue>(jv), fN...);
        return *this;
    }
};

struct XChain_test : public beast::unit_test::suite,
                     public jtx::XChainBridgeObjects
{
    XRPAmount
    reserve(std::uint32_t count)
    {
        return xEnv(*this).env_.current()->fees().accountReserve(count);
    }

    void
    testBridgeCreate()
    {
        XRPAmount res1 = reserve(1);

        using namespace jtx;
        testcase("Bridge Create");

        // Bridge not owned by one of the door account.
        xEnv(*this).tx(create_bridge(mcBob), ter(temSIDECHAIN_NONDOOR_OWNER));

        // Create twice on the same account
        xEnv(*this)
            .tx(create_bridge(mcDoor))
            .close()
            .tx(create_bridge(mcDoor), ter(tecDUPLICATE));

        // Create where both door accounts are on the same chain. The second
        // bridge create should fail.
        xEnv(*this)
            .tx(create_bridge(mcDoor))
            .fund(XRP(10000), scDoor)
            .close()
            .tx(create_bridge(scDoor), ter(tecDUPLICATE));

        // Bridge where the two door accounts are equal.
        Json::Value jvsd = bridge(mcDoor, xrpIssue(), mcDoor, xrpIssue());
        xEnv(*this).tx(
            create_bridge(mcDoor, jvsd), ter(temEQUAL_DOOR_ACCOUNTS));

        // Create an bridge on an account with exactly enough balance to meet
        // the new reserve
        xEnv(*this)
            .fund(res1, mcuDoor)  // exact reserve for account + 1 object
            .close()
            .tx(create_bridge(mcuDoor, jvub), ter(tesSUCCESS));

        // Create an bridge on an account with no enough balance to meet the new
        // reserve
        xEnv(*this)
            .fund(res1 - 1, mcuDoor)  // just short of required reserve
            .close()
            .tx(create_bridge(mcuDoor, jvub), ter(tecINSUFFICIENT_RESERVE));

        // Test all combinations of the following:`
        // --------------------------------------
        // - Locking chain is IOU with locking chain door account as issuer
        // - Locking chain is IOU with issuing chain door account that exists on
        //   the locking chain as issuer
        // - Locking chain is IOU with issuing chain door account that does not
        //   exists on the locking chain as issuer
        // - Locking chain is IOU with non-locking chain door account (that
        //   exists on the locking chain ledger) as issuer
        // - Locking chain is IOU with non-locking chain door account (that does
        //   not exist exists on the locking chain ledger) as issuer
        // - Locking chain is XRP
        // ---------------------------------------------------------------------
        // - Issuing chain is IOU with issuing chain door account as the issuer
        // - Issuing chain is IOU with non-issuing chain door account (that
        //   exists on the ledger issuing chain ledger) as the issuer
        // - Issuing chain is IOU with non-issuing chain door account (that does
        //   not exists on the ledger issuing chain ledger) as the issuer
        // - Issuing chain is IOU with locking chain door account (that exists
        //   on the issuing chain ledger) as the issuer
        // - Issuing chain is IOU with locking chain door account (that deos not
        //   exist exists on the issuing chain ledger) as the issuer
        // - Issuing chain is XRP and issuing chain door account is the root
        //   account
        // - Issuing chain is XRP and issuing chain door account is not the root
        //   account
        // ---------------------------------------------------------------------
        // That's 42 combinations. The only combinations that should succeed
        // are: Locking chain is any IOU, Issuing chain is IOU with issuing
        // chain door account as the issuer Locking chain is XRP, Issuing chain
        // is XRP with issuing chain is the root account.
        // ---------------------------------------------------------------------
        Account a, b;
        Issue ia, ib;

        std::tuple lc{
            [&]() { return xEnv(*this); },
            [&]() { return xEnv(*this); },
            [&]() { return xEnv(*this); }};
    }

    void
    testBridgeModify()
    {
        using namespace jtx;
        testcase("Bridge Modify");
    }

    void
    run() override
    {
        testBridgeCreate();
        // testBridgeModify();
    }
};

BEAST_DEFINE_TESTSUITE(XChain, app, ripple);

}  // namespace test
}  // namespace ripple
