//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2025 Nerd Nest XYZ.

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
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

class Message_test : public beast::unit_test::suite
{
    void
    testEnabled(FeatureBitset features)
    {
        using namespace jtx;
        testcase("Enabled");
        {
            Env env{*this, supported_amendments()};
            Account const alice{"alice"};
            Account const bob{"bob"};
            Account const carol{"carol"};
            env.fund(XRP(10000), alice, bob);
            env.close();

            // Create a ticket.
            Json::Value tx;
            tx[jss::TransactionType] = jss::MessageCreate;
            tx[jss::Account] = alice.human();
            tx[jss::Destination] = carol.human();
            tx[sfMessageData.jsonName] = "DEADBEEF";
            env(tx);
            env.close();

            Json::Value params;
            params[jss::ledger_index] = env.current()->seq() - 1;
            params[jss::transactions] = true;
            params[jss::expand] = true;
            auto const jrr = env.rpc("json", "ledger", to_string(params));
            std::cout << jrr << std::endl;
        }
    }

    void
    testNamespace(FeatureBitset features)
    {
        using namespace jtx;
        testcase("Namespace");
        {
            Env env{*this, supported_amendments()};
            Account const alice{"alice"};
            Account const bob{"bob"};
            env.fund(XRP(10000), alice, bob);
            env.close();

            // Create a namespace.
            {
                Json::Value tx;
                tx[jss::TransactionType] = jss::NamespaceSet;
                tx[jss::Account] = alice.human();
                tx[sfUserName.jsonName] = "7472616E736961";
                tx[sfDisplayName.jsonName] = "44656E697320416E67656C6C";
                env(tx);
                env.close();

                Json::Value params;
                params[jss::ledger_index] = env.current()->seq() - 1;
                params[jss::transactions] = true;
                params[jss::expand] = true;
                auto const jrr = env.rpc("json", "ledger", to_string(params));
                std::cout << jrr << std::endl;
            }

            // Error: No Permission
            {
                Json::Value tx;
                tx[jss::TransactionType] = jss::NamespaceSet;
                tx[jss::Account] = bob.human();
                tx[sfUserName.jsonName] = "7472616E736961";
                env(tx, ter(tecNO_PERMISSION));
                env.close();
            }
        }
    }

public:
    void
    run() override
    {
        using namespace jtx;
        auto const all = supported_amendments();
        testEnabled(all);
        testNamespace(all);
    }
};

BEAST_DEFINE_TESTSUITE(Message, app, ripple);

}  // namespace test
}  // namespace ripple
