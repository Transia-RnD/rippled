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

#include <ripple/protocol/Issue.h>
#include "ripple/protocol/Feature.h"
#include "test/jtx/Env.h"
#include <test/jtx.h>
#include <test/jtx/multisign.h>
#include <test/jtx/sidechain.h>

#include <string>
#include <vector>

namespace ripple {
namespace test {

struct Sidechain_test : public beast::unit_test::suite
{
    void
    testSidechainCreate()
    {
        testcase("Sidechain Create");

        using namespace jtx;
        auto const features =
            supported_amendments() | FeatureBitset{featureSidechains};
        auto const mc_door = Account("mc_door");
        auto const sc_door = Account("sc_door");
        std::vector<signer> const signers = [] {
            constexpr int numSigners = 5;
            std::vector<signer> result;
            result.reserve(numSigners);
            for (int i = 0; i < numSigners; ++i)
            {
                using namespace std::literals;
                auto const a = Account("signer_"s + std::to_string(i));
                result.emplace_back(a);
            }
            return result;
        }();

        {
            Env env(*this, features);
            env.fund(XRP(10000), mc_door);
            std::uint32_t const quorum = signers.size() - 1;
            env(sidechain_create(
                mc_door,
                sidechain(mc_door, xrpIssue(), sc_door, xrpIssue()),
                quorum,
                signers));
        }
        {
            // Test creating when the account is not one of the doors
        }
    }
    void
    run() override
    {
        testSidechainCreate();
    }
};

BEAST_DEFINE_TESTSUITE(Sidechain, app, ripple);

}  // namespace test
}  // namespace ripple
