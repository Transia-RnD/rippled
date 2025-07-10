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

struct Transactor_test : public beast::unit_test::suite
{
    void
    testDID(FeatureBitset features, beast::Journal j)
    {
        // ./rippled -u ripple.bootcamp.Transactor
        testcase("DID");

        using namespace jtx;
        
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(5000), alice);
        env.close();

        JLOG(j.fatal()) << "Testing DID";
        JLOG(j.fatal()) << "--------------------------------------------------";

        env(did::setValid(alice), ter(tesSUCCESS));
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
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        test::SuiteJournal journal("ResourceManager_test", *this);
        testDID(all, journal);
    }
};

BEAST_DEFINE_TESTSUITE(Transactor, bootcamp, ripple);

}  // namespace test
}  // namespace ripple
