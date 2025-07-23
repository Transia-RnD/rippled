//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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

#include <test/jtx.h>

#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

namespace test {

class RecurringPayment_test : public beast::unit_test::suite
{
public:

    Json::Value
    set(
        jtx::Account const& account, 
        STAmount const& amount,
        NetClock::duration const& frequency,
        std::optional<jtx::Account> destination = std::nullopt)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::RecurringPaymentSet;
        jv[jss::Account] = to_string(account.id());
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        jv[sfFrequency.fieldName] = frequency.count();
        if (destination)
            jv[jss::Destination] = to_string(destination->id());

        return jv;
    }

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;
        Env env(*this);
        Account const alice = Account{"alice"};
        Account const bob = Account{"bob"};
        env.fund(XRP(10000), bob, alice);
        env.close();

        auto const frequency = 100s;
        env(set(alice, XRP(1), frequency), ter(tesSUCCESS));
        env.close();

        {
            Json::Value params;
            params[jss::ledger_index] = env.current()->seq() - 1;
            params[jss::transactions] = true;
            params[jss::expand] = true;
            auto const jrr = env.rpc("json", "ledger", to_string(params));
            std::cout << jrr << std::endl;
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();
        testWithFeats(sa);
    }
};
BEAST_DEFINE_TESTSUITE(RecurringPayment, app, ripple);
}  // namespace test
}  // namespace ripple
