//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>

#include <algorithm>

namespace ripple {
namespace test {

struct Logger_test : public beast::unit_test::suite
{
    void
    testPayment(FeatureBitset features)
    {
        // ./rippled -u ripple.bootcamp.Logger
        testcase("Payment");

        using namespace jtx;
        
        Env env{*this, envconfig(), features, nullptr,
            beast::severities::kTrace
        };
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(5000), alice, bob);
        env.close();

        env(pay(alice, bob, XRP(1)), ter(tesSUCCESS));
        env.close();
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testPayment(all);
    }
};

BEAST_DEFINE_TESTSUITE(Logger, bootcamp, ripple);

}  // namespace test
}  // namespace ripple
