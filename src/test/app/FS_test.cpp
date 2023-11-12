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

#include <ripple/basics/strHex.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

#include <algorithm>
#include <iterator>

namespace ripple {
namespace test {

// Helper function that returns the owner count of an account root.
// std::uint32_t
// ownerCount(test::jtx::Env const& env, test::jtx::Account const& acct)
// {
//     std::uint32_t ret{0};
//     if (auto const sleAcct = env.le(acct))
//         ret = sleAcct->at(sfOwnerCount);
//     return ret;
// }

// bool
// checkVL(Slice const& result, std::string expected)
// {
//     Serializer s;
//     s.addRaw(result);
//     return s.getString() == expected;
// }

struct FS_test : public beast::unit_test::suite
{
    // void
    // testEnabled(FeatureBitset features)
    // {
    //     testcase("featureFS Enabled");

    //     using namespace jtx;
    //     // If the DID amendment is not enabled, you should not be able
    //     // to set or delete DIDs.
    //     Env env{*this, features - featureDID};
    //     Account const alice{"alice"};
    //     env.fund(XRP(5000), alice);
    //     env.close();

    //     BEAST_EXPECT(ownerCount(env, alice) == 0);
    //     env(did::setValid(alice), ter(temDISABLED));
    //     env.close();

    //     BEAST_EXPECT(ownerCount(env, alice) == 0);
    //     env(did::del(alice), ter(temDISABLED));
    //     env.close();
    // }

    // void
    // testAccountReserve(FeatureBitset features)
    // {
    //     // Verify that the reserve behaves as expected for minting.
    //     testcase("DID Account Reserve");

    //     using namespace test::jtx;

    //     Env env{*this, features};
    //     Account const alice{"alice"};

    //     // Fund alice enough to exist, but not enough to meet
    //     // the reserve for creating a DID.
    //     auto const acctReserve = env.current()->fees().accountReserve(0);
    //     auto const incReserve = env.current()->fees().increment;
    //     env.fund(acctReserve, alice);
    //     env.close();
    //     BEAST_EXPECT(env.balance(alice) == acctReserve);
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // alice does not have enough XRP to cover the reserve for a DID
    //     env(did::setValid(alice), ter(tecINSUFFICIENT_RESERVE));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // Pay alice almost enough to make the reserve for a DID.
    //     env(pay(env.master, alice, incReserve + drops(19)));
    //     BEAST_EXPECT(env.balance(alice) == acctReserve + incReserve + drops(9));
    //     env.close();

    //     // alice still does not have enough XRP for the reserve of a DID.
    //     env(did::setValid(alice), ter(tecINSUFFICIENT_RESERVE));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // Pay alice enough to make the reserve for a DID.
    //     env(pay(env.master, alice, drops(11)));
    //     env.close();

    //     // Now alice can create a DID.
    //     env(did::setValid(alice));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 1);

    //     // alice deletes her DID.
    //     env(did::del(alice));
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);
    //     env.close();
    // }

    // void
    // testPinInvalid(FeatureBitset features)
    // {
    //     testcase("Invalid FSPin");

    //     using namespace jtx;
    //     using namespace std::chrono;

    //     Env env(*this);
    //     Account const alice{"alice"};
    //     env.fund(XRP(5000), alice);
    //     env.close();

    //     //----------------------------------------------------------------------
    //     // preflight

    //     // invalid flags
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);
    //     env(did::setValid(alice), txflags(0x00010000), ter(temINVALID_FLAG));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // no fields
    //     env(did::set(alice), ter(temEMPTY_DID));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // all empty fields
    //     env(did::set(alice),
    //         did::uri(""),
    //         did::document(""),
    //         did::data(""),
    //         ter(temEMPTY_DID));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // uri is too long
    //     const std::string longString(257, 'a');
    //     env(did::set(alice), did::uri(longString), ter(temMALFORMED));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // document is too long
    //     env(did::set(alice), did::document(longString), ter(temMALFORMED));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // attestation is too long
    //     env(did::set(alice),
    //         did::document("data"),
    //         did::data(longString),
    //         ter(temMALFORMED));
    //     env.close();
    //     BEAST_EXPECT(ownerCount(env, alice) == 0);

    //     // Modifying a DID to become empty is checked in testSetModify
    // }

    static Json::Value
    fs_pin(jtx::Account const& account)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::FSPin;
        jv[jss::Account] = to_string(account.id());
        jv[jss::Flags] = tfUniversal;
        return jv;
    }

    void
    testPin(FeatureBitset features)
    {
        testcase("FSPin");

        using namespace jtx;
        using namespace std::chrono;

        Env env(*this);

        // Env env{*this, envconfig(), supported_amendments(), nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace
        // };

        Account const alice{"alice"};
        env.fund(XRP(5000), alice);
        env.close();

        auto tx = fs_pin(alice);
        tx[sfData.jsonName] = "DEADBEEF";
        env(tx, ter(tesSUCCESS));
        env.close();

        Json::Value params;
        params[jss::transaction] = env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto jrr = env.rpc("json", "tx", to_string(params))[jss::result];
        std::cout << "RESULT: " << jrr << "\n";
        
        // Json::Value jv;
        // jv[jss::ledger_index] = "current";
        // jv[jss::queue] = true;
        // jv[jss::expand] = true;

        // auto jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testPin(all);
        // testEnabled(all);
        // testAccountReserve(all);
        // testSetInvalid(all);
    }
};

BEAST_DEFINE_TESTSUITE(FS, app, ripple);

}  // namespace test
}  // namespace ripple
