//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2025 Ripple Labs Inc.

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

#include <xrpld/ledger/Dir.h>

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {
struct Firewall_test : public beast::unit_test::suite
{
    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    };

    static std::pair<uint256, std::shared_ptr<SLE const>>
    firewallKeyAndSle(ReadView const& view, jtx::Account const& account)
    {
        auto const k = keylet::firewall(account);
        return {k.key, view.read(k)};
    }

    void
    testFirewallBlock(FeatureBitset features)
    {
        testcase("firewall block");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            auto const seq = env.seq(alice);
            auto const fee = env.current()->fees().base;
            env(firewall::set(alice, seq, fee),
                firewall::backup(bob),
                firewall::counter_party(carol),
                firewall::next_otp(escrow::cb1),
                ter(tesSUCCESS));
            env.close();

            {
                Json::Value params;
                params[jss::ledger_index] = env.current()->seq() - 1;
                params[jss::transactions] = true;
                params[jss::expand] = true;
                auto const jrr = env.rpc("json", "ledger", to_string(params));
                std::cout << jrr << std::endl;
            }

            env(pay(alice, dave, XRP(1)), ter(tecFIREWALL_BLOCK));
            env.close();

            env(pay(alice, dave, XRP(1)), firewall::next_otp(escrow::cb2), firewall::otp(escrow::fb1), ter(tesSUCCESS));
            env.close();

            env(pay(alice, bob, XRP(1)), ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testFirewallBlock(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{testable_amendments()};
        testWithFeats(all);
    }
};

BEAST_DEFINE_TESTSUITE(Firewall, app, ripple);
}  // namespace test
}  // namespace ripple