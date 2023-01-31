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

#include <ripple/basics/chrono.h>
#include <ripple/ledger/Directory.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
// #include <ripple/protocol/URIToken.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

#include <chrono>

namespace ripple {
namespace test {
struct URIToken_test : public beast::unit_test::suite
{
    static uint256
    tokenid(
        jtx::Account const& account,
        std::string const& uri)
    {
        auto const k = keylet::uritoken(account, Blob(uri.begin(), uri.end()));
        return k.key;
    }

    // static std::pair<uint256, std::shared_ptr<SLE const>>
    // uriTokenKeyAndSle(
    //     ReadView const& view,
    //     jtx::Account const& account,
    //     std::string const& uri)
    // {
    //     auto const k = keylet::uritoken(account, uri);
    //     return {k.key, view.read(k)};
    // }

    static Json::Value
    mint(
        jtx::Account const& account,
        std::string const& uri)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Flags] = tfBurnable;
        jv[jss::Account] = account.human();
        jv[sfURI.jsonName] = strHex(uri);
        return jv;
    }

    static Json::Value
    burn(
        jtx::Account const& account,
        std::string const& id)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Flags] = tfBurn;
        jv[jss::Account] = account.human();
        jv[sfURITokenID.jsonName] = id;
        return jv;
    }

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;
        
        // setup env
        auto const alice = Account("alice");
        auto USD = alice["USD"];
        
        {
            // If the URIToken amendment is not enabled, you should not be able
            // to mint, burn, buy, sell or clear uri tokens.
            Env env{*this, features - featureURIToken};

            env.fund(XRP(1000), alice);

            // MINT
            std::string const uri(maxTokenURILength, '?');
            env(mint(alice, uri), ter(temDISABLED));
            env.close();

            // BURN
            std::string const uriTokenID{strHex(tokenid(alice, uri))};
            env(burn(alice, uriTokenID), ter(temDISABLED));
            env.close();

            // BUY

            // SELL

            // CLEAR
        }
        {
            // If the URIToken amendment is enabled, you should be able
            // to mint, burn, buy, sell or clear uri tokens.
            Env env{*this, features};

            env.fund(XRP(1000), alice);

            // MINT
            std::string const uri(maxTokenURILength, '?');
            env(mint(alice, uri));
            env.close();

            // BURN
            std::string const uriTokenID{strHex(tokenid(alice, uri))};
            // std::cout << "URI TOKEN ID: " << uriTokenID << "\n";
            env(burn(alice, uriTokenID));
            env.close();

            // BUY

            // SELL

            // CLEAR
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
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(URIToken, app, ripple);
}  // namespace test
}  // namespace ripple
