//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2016 Ripple Labs Inc.

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
#include <test/jtx/WSClient.h>
#include <test/rpc/GRPCTestClientBase.h>

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

class RPC_test : public beast::unit_test::suite
{
public:
    void
    testAccountInfo(FeatureBitset const& features)
    {
        testcase("Account Info");
        using namespace jtx;

        Env env(*this, features);
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);

        {
            Json::Value params;
            params[jss::account] = alice.human();
            auto const jrr = env.rpc("json", "account_info", to_string(params));
            std::cout << jrr << std::endl;
        }
    }

    void
    testAccountObjects(FeatureBitset const& features)
    {
        testcase("Account Objects");
        using namespace jtx;

        Env env(*this, features);
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);

        // Create an object for Alice
        env(offer(alice, bob["USD"](100), XRP(10)));

        {
            Json::Value params;
            params[jss::account] = alice.human();
            auto const jrr = env.rpc("json", "account_objects", to_string(params));
            std::cout << jrr << std::endl;
        }
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testAccountInfo(all);
        // testAccountObjects(all);
    }
};

BEAST_DEFINE_TESTSUITE(RPC, bootcamp, ripple);

}  // namespace test
}  // namespace ripple
