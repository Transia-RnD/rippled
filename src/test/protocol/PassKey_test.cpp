//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs.

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

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {
class PassKey_test : public beast::unit_test::suite
{

    Json::Value
    passkeyListSet(jtx::Account const& account)
    {
        Json::Value jv;
        jv[sfAccount.jsonName] = account.human();
        jv[sfTransactionType.jsonName] = jss::PasskeyListSet;
        jv[sfPasskeys] = Json::arrayValue;
        jv[sfPasskeys][0u][sfPasskey][sfPasskeyID] = "DEADBEEF";
        jv[sfPasskeys][0u][sfPasskey][sfPublicKey] = strHex(account.pk());
        return jv;
    }

    void
    testP256(FeatureBitset features)
    {
        using namespace test::jtx;

        testcase("p256");

        Env env{*this, envconfig(), features};
        Account const alice{"alice", KeyType::p256};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        env(pay(alice, bob, XRP(100)));
        env.close();

        Json::Value params;
        params[jss::ledger_index] = env.current()->seq() - 1;
        params[jss::transactions] = true;
        params[jss::expand] = true;
        auto const jrr = env.rpc("json", "ledger", to_string(params));
        std::cout << jrr << std::endl;
    }

    void
    testSimplePayment(FeatureBitset features)
    {
        using namespace test::jtx;

        testcase("simple payment");

        Env env{*this, envconfig(), features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const dave{"dave", KeyType::p256};
        env.fund(XRP(1000), alice, bob, dave);
        env.close();

        env(passkeyListSet(alice));
        env(pay(alice, bob, XRP(100)), sig(dave));
        env.close();

        Json::Value params;
        params[jss::ledger_index] = env.current()->seq() - 1;
        params[jss::transactions] = true;
        params[jss::expand] = true;
        auto const jrr = env.rpc("json", "ledger", to_string(params));
        std::cout << jrr << std::endl;
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testP256(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(PassKey, protocol, ripple);
}  // namespace test
}  // namespace ripple