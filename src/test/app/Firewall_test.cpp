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

    void
    verifyFirewallSle(
        ReadView const& view,
        jtx::Account const& account,
        jtx::Account const& issuer,
        std::optional<STAmount> const& amount = std::nullopt,
        std::optional<uint32_t> const& timePeriod = std::nullopt,
        std::optional<uint32_t> const& timeStart = std::nullopt,
        std::optional<STAmount> const& totalOut = std::nullopt)
    {
        using namespace jtx;
        auto [key, sle] = firewall::keyAndSle(view, account);
        BEAST_EXPECT((*sle)[sfOwner] == account.id());
        BEAST_EXPECT((*sle)[sfIssuer] == issuer.id());
    }

    void
    testSetPreflightCreate(FeatureBitset features)
    {
        testcase("set preflight create");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");

        // temDISABLED: Amendment not enabled
        {
            auto const amend = features - featureFirewall;
            Env env{*this, amend};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(temDISABLED));
        }

        // Basic Preflight1 Checks
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // temINVALID_FLAG: Invalid flags set
            auto jt = firewall::set(alice);
            env(jt,
                firewall::backup(bob),
                firewall::counter_party(carol),
                txflags(tfClose),
                ter(temINVALID_FLAG));

            // temBAD_FEE: Invalid fee amount (test with 0 fee)
            env(firewall::set(alice),
                fee(XRP(-1)),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(temBAD_FEE));
        }

        // Required Fields
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // temMALFORMED: FirewallSet: sfCounterParty is required for
            // creation
            env(firewall::set(alice), firewall::backup(bob), ter(temMALFORMED));

            // temMALFORMED: FirewallSet: sfBackup is required for creation
            env(firewall::set(alice),
                firewall::counter_party(carol),
                ter(temMALFORMED));
        }

        // Forbidden Fields
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // temMALFORMED: FirewallSet: sfFirewallSigners not allowed for
            // creation
            env(firewall::set(alice),
                firewall::counter_party(carol),
                firewall::backup(bob),
                firewall::sig(alice),
                ter(temMALFORMED));
        }

        // Edge Cases
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // // Test with exactly 8 rules (maximum allowed) - should succeed
            // auto jt = firewall::set(alice);
            // for (int i = 0; i < 8; ++i)
            // {
            //     jt[jss::FirewallRules][i][jss::FirewallRule]
            //       [sfLedgerEntryType.jsonName] = ltACCOUNT_ROOT;
            //     jt[jss::FirewallRules][i][jss::FirewallRule]
            //       [sfFieldCode.jsonName] = sfBalance.fieldCode;
            //     jt[jss::FirewallRules][i][jss::FirewallRule]
            //       [sfComparisonOperator.jsonName] = 4;
            //     jt[jss::FirewallRules][i][jss::FirewallRule][sfFirewallValue]
            //       [jss::type] = "AMOUNT";
            //     jt[jss::FirewallRules][i][jss::FirewallRule][sfFirewallValue]
            //       [jss::value] = XRP(10 +
            //       i).value().getJson(JsonOptions::none);
            // }
            // env(jt,
            //     firewall::backup(bob),
            //     firewall::counter_party(carol),
            //     ter(tesSUCCESS));

            // Test with all comparison operators (1-5)
            // for (int op = 1; op <= 5; ++op)
            // {
            //     jt = firewall::set(alice);
            //     jt[jss::FirewallRules][0u][jss::FirewallRule]
            //       [sfLedgerEntryType.jsonName] = ltACCOUNT_ROOT;
            //     jt[jss::FirewallRules][0u][jss::FirewallRule]
            //       [sfFieldCode.jsonName] = sfBalance.fieldCode;
            //     jt[jss::FirewallRules][0u][jss::FirewallRule]
            //       [sfComparisonOperator.jsonName] = op;
            //     jt[jss::FirewallRules][0u][jss::FirewallRule][sfFirewallValue]
            //       [jss::type] = "AMOUNT";
            //     jt[jss::FirewallRules][0u][jss::FirewallRule][sfFirewallValue]
            //       [jss::value] = XRP(10).value().getJson(JsonOptions::none);
            //     env(jt,
            //         firewall::backup(bob),
            //         firewall::counter_party(carol),
            //         ter(tesSUCCESS));
            // }

            // Test with boundary values (0 amount)
            // jt = firewall::set(alice);
            // jt[jss::FirewallRules][0u][jss::FirewallRule]
            //   [sfLedgerEntryType.jsonName] = ltACCOUNT_ROOT;
            // jt[jss::FirewallRules][0u][jss::FirewallRule]
            //   [sfFieldCode.jsonName] = sfBalance.fieldCode;
            // jt[jss::FirewallRules][0u][jss::FirewallRule]
            //   [sfComparisonOperator.jsonName] = 4;
            // jt[jss::FirewallRules][0u][jss::FirewallRule][sfFirewallValue]
            //   [jss::type] = "AMOUNT";
            // jt[jss::FirewallRules][0u][jss::FirewallRule][sfFirewallValue]
            //   [jss::value] = XRP(0).value().getJson(JsonOptions::none);
            // env(jt,
            //     firewall::backup(bob),
            //     firewall::counter_party(carol),
            //     ter(tesSUCCESS));

            // // Test with maximum XRP amount
            // jt = firewall::set(alice);
            // jt[jss::FirewallRules][0u][jss::FirewallRule]
            //   [sfLedgerEntryType.jsonName] = ltACCOUNT_ROOT;
            // jt[jss::FirewallRules][0u][jss::FirewallRule]
            //   [sfFieldCode.jsonName] = sfBalance.fieldCode;
            // jt[jss::FirewallRules][0u][jss::FirewallRule]
            //   [sfComparisonOperator.jsonName] = 4;
            // jt[jss::FirewallRules][0u][jss::FirewallRule][sfFirewallValue]
            //   [jss::type] = "AMOUNT";
            // jt[jss::FirewallRules][0u][jss::FirewallRule][sfFirewallValue]
            //   [jss::value] =
            //       STAmount(STAmount::cMaxNative).getJson(JsonOptions::none);
            // env(jt,
            //     firewall::backup(bob),
            //     firewall::counter_party(carol),
            //     ter(tesSUCCESS));
        }

        // tesSUCCESS: Valid create with CounterParty and Backup only
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testSetPreflightUpdate(FeatureBitset features)
    {
        testcase("set preflight update");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // Required Fields
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // temMALFORMED: FirewallSet: sfFirewallSigners required for updates
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            env(jt, ter(temMALFORMED));

            // temMALFORMED: FirewallSet: sfFirewallID required for updates
            // (when missing)
            env(firewall::set(alice), firewall::sig(carol), ter(temMALFORMED));
        }

        // Forbidden Fields
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // temMALFORMED: FirewallSet: sfBackup not allowed for updates
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            env(jt,
                firewall::backup(dave),
                firewall::sig(carol),
                ter(temMALFORMED));
        }

        // FirewallSigners Validation
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);

            // temMALFORMED: FirewallSet: sfFirewallSigners cannot be empty
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfFirewallSigners] = Json::arrayValue;  // Empty array
            env(jt, ter(temMALFORMED));

            // temMALFORMED: FirewallSet: sfFirewallSigners cannot include the
            // outer account
            jt = firewall::set(alice, firewallKey, seq, fee);
            env(jt, firewall::sig(alice), ter(temMALFORMED));

            // temBAD_SIGNATURE: FirewallSet: invalid firewall signature
            jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfFirewallSigners][0u][sfFirewallSigner][jss::Account] =
                carol.human();
            jt[sfFirewallSigners][0u][sfFirewallSigner][jss::SigningPubKey] =
                strHex(carol.pk().slice());
            jt[sfFirewallSigners][0u][sfFirewallSigner][jss::TxnSignature] =
                "deadbeef";
            env(jt, ter(temBAD_SIGNATURE));
        }

        // tesSUCCESS: Valid update with proper single signature
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
            env.close();
        }

        // tesSUCCESS: Valid update with new CounterParty
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfCounterParty.jsonName] = dave.human();
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
        }
    }

    void
    testSetPreclaimCreate(FeatureBitset features)
    {
        testcase("set preclaim create");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // Duplicate Check
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create a firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // tecDUPLICATE: FirewallSet: Firewall already exists for account
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tecDUPLICATE));
        }

        // CounterParty Account Existence
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();
            env.memoize(carol);

            // tecNO_DST: FirewallSet: CounterParty account does not exist
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),  // carol was never funded
                ter(tecNO_DST));
        }

        // Backup Account Existence
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, carol);
            env.close();
            env.memoize(bob);

            // tecNO_DST: FirewallSet: Backup account does not exist
            env(firewall::set(alice),
                firewall::backup(bob),  // bob was never funded
                firewall::counter_party(carol),
                ter(tecNO_DST));
        }

        // // Reserve Requirements
        // {
        //     Env env{*this, features};
        //     env.fund(XRP(1000), alice, bob, carol);
        //     env.close();

        //     // Drain alice's balance to near reserve
        //     auto const reserve = env.current()->fees().accountReserve(0);
        //     auto const baseFee = env.current()->fees().base;
        //     auto const firewallReserve =
        //     env.current()->fees().accountReserve(2); // +2 for firewall +
        //     preauth

        //     // Leave just enough for current reserve but not enough for
        //     firewall + preauth env(pay(alice, bob,
        //         alice.balance(*env.current()) - reserve - baseFee));
        //     env.close();

        //     // tecINSUFFICIENT_RESERVE: Insufficient reserve for Firewall +
        //     WithdrawPreauth env(firewall::set(alice),
        //         firewall::backup(bob),
        //         firewall::counter_party(carol),
        //         ter(tecINSUFFICIENT_RESERVE));
        // }
    }

    void
    testSetPreclaimUpdate(FeatureBitset features)
    {
        testcase("set preclaim update");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");
        Account const eve = Account("eve");

        // Firewall Existence
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // tecNO_TARGET: FirewallSet: Firewall not found
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(
                alice, uint256{1}, seq, fee);  // Non-existent firewall ID
            env(jt, firewall::sig(carol), ter(tecNO_TARGET));
        }

        // Permission Check
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // tecNO_PERMISSION: FirewallSet: Account is not the firewall owner
            auto const seq = env.seq(dave);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(
                dave,
                firewallKey,
                seq,
                fee);  // dave trying to update alice's firewall
            env(jt, firewall::sig(carol), ter(tecNO_PERMISSION));
        }

        // New CounterParty Validation - Same as existing
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // tecDUPLICATE: FirewallSet: sfCounterParty must not be the same as
            // existing CounterParty
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfCounterParty.jsonName] = carol.human();  // Same as existing
            env(jt, firewall::sig(carol), ter(tecDUPLICATE));
        }

        // UNew CounterParty Validation - Account does not exist
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // tecNO_DST: FirewallSet: New CounterParty account does not exist
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfCounterParty.jsonName] = eve.human();  // eve was never funded
            env(jt, firewall::sig(carol), ter(tecNO_DST));
        }

        // Valid cases - Update CounterParty only
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // tesSUCCESS: Valid update changing CounterParty
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfCounterParty.jsonName] =
                dave.human();  // Different from existing carol
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
        }

        // Valid cases - No changes (update with no new fields)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create a firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // tesSUCCESS: Valid update with no changes (just signature)
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
        }
    }

    void
    testSetDoApplyCreate(FeatureBitset features)
    {
        testcase("set doapply create");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // tesSUCCESS
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create firewall
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify Firewall SLE created correctly
            auto const sleFirewall =
                env.current()->read(keylet::firewall(alice));
            BEAST_EXPECT(sleFirewall);
            BEAST_EXPECT(sleFirewall->getAccountID(sfOwner) == alice.id());
            BEAST_EXPECT(
                sleFirewall->getAccountID(sfCounterParty) == carol.id());
            BEAST_EXPECT(sleFirewall->isFieldPresent(sfOwnerNode));

            // Verify WithdrawPreauth SLE created for backup
            auto const slePreauth =
                env.current()->read(keylet::withdrawPreauth(alice, bob, 0));
            BEAST_EXPECT(slePreauth);
            BEAST_EXPECT(slePreauth->getAccountID(sfAccount) == alice.id());
            BEAST_EXPECT(slePreauth->getAccountID(sfAuthorize) == bob.id());
            BEAST_EXPECT(slePreauth->isFieldPresent(sfOwnerNode));
            BEAST_EXPECT(slePreauth->getFieldU32(sfDestinationTag) == 0);

            // Verify owner count increased by 2
            auto const sleOwner = env.current()->read(keylet::account(alice));
            BEAST_EXPECT(sleOwner->getFieldU32(sfOwnerCount) == 2);
        }

        // CREATE - Directory full for firewall
        // NOT TESTED: Directory full for preauth (would require additional
        // accounts)

        // // tecINSUFFICIENT_RESERVE - Insufficient reserve for firewall +
        // preauth
        // {
        //     Env env{*this, features};
        //     env.fund(XRP(1000), alice, bob, carol);
        //     env.close();

        //     // Calculate reserves
        //     auto const reserve = env.current()->fees().accountReserve(0);
        //     auto const baseFee = env.current()->fees().base;

        //     // Drain alice's balance to just above base reserve
        //     env(
        //         pay(alice,
        //             bob,
        //             alice.balance(*env.current()) - reserve - baseFee * 2));
        //     env.close();

        //     // Attempt to create firewall with insufficient reserve
        //     env(firewall::set(alice),
        //         firewall::backup(bob),
        //         firewall::counter_party(carol),
        //         ter(tecINSUFFICIENT_RESERVE));
        // }
    }

    void
    testSetDoApplyUpdate(FeatureBitset features)
    {
        testcase("set doapply update");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");
        Account const eve = Account("eve");

        // tesSUCCESS
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall first
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            // Update CounterParty from carol to dave
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfCounterParty.jsonName] = dave.human();
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
            env.close();

            // Verify CounterParty updated
            auto const updatedSle =
                env.current()->read(keylet::firewall(alice));
            BEAST_EXPECT(updatedSle->getAccountID(sfCounterParty) == dave.id());
            BEAST_EXPECT(
                updatedSle->getAccountID(sfOwner) == alice.id());  // Unchanged
        }

        // tesSUCCESS - Update with no changes (signature only)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create firewall
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, firewallSle] =
                firewall::keyAndSle(*env.current(), alice);

            auto const prevCounterParty =
                firewallSle->getAccountID(sfCounterParty);

            // Update with no actual changes
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
            env.close();

            // Verify nothing changed except metadata
            auto const updatedSle =
                env.current()->read(keylet::firewall(alice));
            BEAST_EXPECT(
                updatedSle->getAccountID(sfCounterParty) == prevCounterParty);
        }
    }

    void
    testMasterKeyDisable(FeatureBitset features)
    {
        testcase("master key disable");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");

        // SetAccount with asfDisableMaster blocked by firewall
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Set Regular Key
            env(regkey(alice, bob), ter(tesSUCCESS));

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Attempt to disable master key - should be blocked
            env(fset(alice, asfDisableMaster),
                sig(alice),
                ter(tecNO_PERMISSION));
        }

        // SetAccount with asfDisableMaster without firewall - succeeds
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            // Set Regular Key
            env(regkey(alice, bob), ter(tesSUCCESS));

            // Disable master key without firewall - should succeed
            env(fset(alice, asfDisableMaster), sig(alice), ter(tesSUCCESS));
        }

        // Other SetAccount flags work with firewall present
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Other flags should work fine
            env(fset(alice, asfRequireDest), ter(tesSUCCESS));
            env(fclear(alice, asfRequireDest), ter(tesSUCCESS));
        }

        // Delete firewall then disable master key - should succeed
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Set Regular Key first
            env(regkey(alice, bob), ter(tesSUCCESS));
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify master key cannot be disabled while firewall exists
            env(fset(alice, asfDisableMaster),
                sig(alice),
                ter(tecNO_PERMISSION));
            env.close();

            // Get firewall key for deletion
            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Delete the firewall
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(carol),
                sig(alice),
                ter(tesSUCCESS));
            env.close();

            // Verify firewall is deleted
            BEAST_EXPECT(!env.current()->exists(keylet::firewall(alice)));

            // Now disable master key should succeed
            env(fset(alice, asfDisableMaster), sig(alice), ter(tesSUCCESS));
            env.close();

            // Verify master key is disabled
            auto const sleAccount = env.current()->read(keylet::account(alice));
            BEAST_EXPECT(sleAccount->isFlag(lsfDisableMaster));

            // Verify alice can no longer sign with master key
            env(noop(alice), sig(alice), ter(tefMASTER_DISABLED));

            // But can still sign with regular key
            env(noop(alice), sig(bob), ter(tesSUCCESS));
        }
    }

    void
    testTransactionType(FeatureBitset features)
    {
        testcase("transaction type");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // Payment blocked without preauth
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Payment from alice to dave should be blocked (no preauth)
            env.fund(XRP(1000), dave);
            env.close();
            env(pay(alice, dave, XRP(10)), ter(tecFIREWALL_BLOCK));

            // Payment from dave to alice should succeed (incoming)
            env(pay(dave, alice, XRP(10)), ter(tesSUCCESS));
        }

        // Payment blocked without preauth (dtag)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                dtag(1),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Payment from alice to dave should be blocked (no preauth)
            env.fund(XRP(1000), dave);
            env.close();
            env(pay(alice, dave, XRP(10)), ter(tecFIREWALL_BLOCK));

            // Payment from alice to bob w/o dtag should be blocked (no dtag)
            env(pay(alice, bob, XRP(10)), ter(tecFIREWALL_BLOCK));
        }

        // Payment allowed with preauth
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Bob has preauth (as backup), payment should succeed
            env(pay(bob, alice, XRP(10)), ter(tesSUCCESS));

            // Add preauth for dave
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Now alice can pay dave
            env(pay(alice, dave, XRP(10)), ter(tesSUCCESS));
        }

        // Payment allowed with preauth (dtag)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                dtag(1),
                ter(tesSUCCESS));
            env.close();

            // Alice has preauth bob (as backup), payment should succeed
            env(pay(alice, bob, XRP(10)), dtag(1), ter(tesSUCCESS));

            // Add preauth for dave
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                dtag(1),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Now alice can pay dave
            env(pay(alice, dave, XRP(10)), dtag(1), ter(tesSUCCESS));
        }

        // Offer operations with firewall
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();
            auto const USD = bob["USD"];
            env.trust(USD(1000), alice, dave);
            env(pay(bob, alice, USD(1000)));
            env(pay(bob, dave, USD(1000)));

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Alice creates an offer
            env(offer(alice, USD(10), XRP(10)), ter(tesSUCCESS));

            // Dave tries to cross alice's offer - blocked
            env(offer(dave, XRP(10), USD(10)), ter(tecFIREWALL_BLOCK));

            // Bob can cross alice's offer
            env(offer(bob, XRP(10), USD(10)), ter(tesSUCCESS));
        }

        // Escrow operations with firewall
        {
            using namespace std::chrono;
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Alice creates escrow for dave - should be blocked
            env(escrow::create(alice, dave, XRP(50)),
                escrow::finish_time(env.now() + 10s),
                ter(tecFIREWALL_BLOCK));

            // Alice creates escrow for bob - should succeed
            env(escrow::create(alice, bob, XRP(50)),
                escrow::finish_time(env.now() + 10s),
                ter(tesSUCCESS));
        }

        // Payment Channel operations
        {
            using namespace std::chrono;
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Alice tries to create payment channel to dave - blocked
            env(create(alice, dave, XRP(100), 10s, alice.pk()),
                ter(tecFIREWALL_BLOCK));

            // Bob creates payment channel to alice - succeeds
            env(create(alice, bob, XRP(100), 10s, alice.pk()), ter(tesSUCCESS));
        }

        // Check operations
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Alice tries to check dave - blocked
            uint256 checkID = keylet::check(alice, env.seq(alice)).key;
            env(check::create(alice, dave, XRP(10)), ter(tesSUCCESS));

            // Dave tries to cash check from alice - blocked
            env(check::cash(dave, checkID, XRP(10)), ter(tecFIREWALL_BLOCK));
        }

        // NFT Sell operations with firewall (bad destination)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Alice mints NFT
            uint256 nftID = token::getNextID(env, alice, 0u);
            env(token::mint(alice), ter(tesSUCCESS));
            env.close();

            // Alice tries to create a sell offer her NFT w/ amount - not
            // blocked
            uint256 const aliceOfferIndex =
                keylet::nftoffer(alice, env.seq(alice)).key;
            env(token::createOffer(alice, nftID, XRP(10)),
                token::destination(dave),
                txflags(tfSellNFToken),
                ter(tesSUCCESS));
            env.close();

            // Dave tries to accept alice's offer - blocked
            env(token::acceptSellOffer(dave, aliceOfferIndex),
                ter(tecFIREWALL_BLOCK));
            env.close();
        }

        // NFT Sell operations with firewall (good destination)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Alice mints NFT
            uint256 nftID = token::getNextID(env, alice, 0u);
            env(token::mint(alice), ter(tesSUCCESS));
            env.close();

            // Alice tries to create a sell offer her NFT w/ amount - not
            // blocked
            uint256 const aliceOfferIndex =
                keylet::nftoffer(alice, env.seq(alice)).key;
            env(token::createOffer(alice, nftID, XRP(10)),
                token::destination(bob),
                txflags(tfSellNFToken),
                ter(tesSUCCESS));
            env.close();

            env(token::acceptSellOffer(bob, aliceOfferIndex), ter(tesSUCCESS));
            env.close();
        }

        // NFT Buy operations with firewall (bad destination)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Dave mints NFT
            uint256 nftID = token::getNextID(env, dave, 0u, tfTransferable);
            env(token::mint(dave), txflags(tfTransferable), ter(tesSUCCESS));
            env.close();

            // Alice tries to create offer for dave's NFT - blocked
            uint256 const aliceOfferIndex =
                keylet::nftoffer(alice, env.seq(alice)).key;
            env(token::createOffer(alice, nftID, XRP(10)),
                token::owner(dave),
                ter(tesSUCCESS));
            env.close();

            env(token::acceptBuyOffer(dave, aliceOfferIndex),
                ter(tecFIREWALL_BLOCK));
            env.close();
        }

        // NFT Buy operations with firewall (good destination)
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Dave mints NFT
            uint256 nftID = token::getNextID(env, bob, 0u, tfTransferable);
            env(token::mint(bob), txflags(tfTransferable), ter(tesSUCCESS));
            env.close();

            // Alice tries to create offer for bob's NFT - not blocked
            uint256 const aliceOfferIndex =
                keylet::nftoffer(alice, env.seq(alice)).key;
            env(token::createOffer(alice, nftID, XRP(10)),
                token::owner(bob),
                ter(tesSUCCESS));
            env.close();

            env(token::acceptBuyOffer(bob, aliceOfferIndex), ter(tesSUCCESS));
            env.close();
        }

        // MPT operations with firewall
        {
            Env env{*this, features};
            auto const alice = Account("alice");
            auto const gw = Account("gw");
            MPTTester mptGw(env, gw, {.holders = {alice, dave, bob}});
            mptGw.create(
                {.ownerCount = 1,
                 .holderCount = 0,
                 .flags = tfMPTCanEscrow | tfMPTCanTransfer});
            mptGw.authorize({.account = alice});
            mptGw.authorize({.account = dave});
            mptGw.authorize({.account = bob});
            auto const MPT = mptGw["MPT"];
            env(pay(gw, alice, MPT(10'000)));
            env(pay(gw, dave, MPT(10'000)));
            env(pay(gw, bob, MPT(10'000)));
            env.close();

            env.fund(XRP(1000), carol);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            env(pay(alice, dave, MPT(10)), ter(tecFIREWALL_BLOCK));
            env.close();

            env(pay(alice, bob, MPT(10)), ter(tesSUCCESS));
            env.close();
        }

        // Non-payment transactions should work normally
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Attempt to drain account with high fee
            // KNOWN ISSUE: There is no way to block non payment transaction
            // that attempt to drain a fee
            env(noop(alice), fee(XRP(50)), ter(tesSUCCESS));

            // Regular key operations work
            env(regkey(alice, dave), ter(tesSUCCESS));

            // Signer list operations work
            env(signers(alice, 1, {{bob, 1}}), ter(tesSUCCESS));
        }
    }

    void
    testDeletePreflight(FeatureBitset features)
    {
        testcase("delete preflight");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");

        // temDISABLED: Amendment not enabled
        {
            auto const amend = features - featureFirewall;
            Env env{*this, amend};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, uint256{1}, seq, fee),
                firewall::sig(carol),
                ter(temDISABLED));
        }

        // temINVALID_FLAG: Invalid flags set
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // First create a firewall
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(carol),
                txflags(tfClose),
                ter(temINVALID_FLAG));
        }

        // temMALFORMED: Missing FirewallSigners
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 0);
            auto jt = firewall::del(alice, firewallKey, seq, fee);
            // Remove FirewallSigners field
            jt.removeMember(sfFirewallSigners.jsonName);
            env(jt, ter(temMALFORMED));
        }

        // temBAD_SIGNATURE: Invalid signature
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = firewall::del(alice, firewallKey, seq, fee);
            jt[sfFirewallSigners][0u][sfFirewallSigner][jss::Account] =
                carol.human();
            jt[sfFirewallSigners][0u][sfFirewallSigner][jss::SigningPubKey] =
                strHex(carol.pk().slice());
            jt[sfFirewallSigners][0u][sfFirewallSigner][jss::TxnSignature] =
                "deadbeef";
            env(jt, ter(temBAD_SIGNATURE));
        }
    }

    void
    testDeletePreclaim(FeatureBitset features)
    {
        testcase("delete preclaim");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // tecNO_TARGET: Firewall doesn't exist
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, uint256{1}, seq, fee),
                firewall::sig(carol),
                ter(tecNO_TARGET));
        }

        // tecNO_PERMISSION: Not the firewall owner
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Dave tries to delete alice's firewall
            auto const seq = env.seq(dave);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tecNO_PERMISSION));
        }

        // tefBAD_AUTH: Wrong signer
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Try to delete with wrong signer (dave instead of carol)
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(dave),
                ter(tefBAD_AUTH));
        }

        // tesSUCCESS: Valid delete with correct signer
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
        }
    }

    void
    testDeleteDoApply(FeatureBitset features)
    {
        testcase("delete doapply");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");
        Account const eve = Account("eve");

        // Basic delete with only initial backup preauth
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create firewall (creates preauth for bob as backup)
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify firewall and preauth exist
            BEAST_EXPECT(env.current()->exists(keylet::firewall(alice)));
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));

            auto const ownerCountBefore = env.current()
                                              ->read(keylet::account(alice))
                                              ->getFieldU32(sfOwnerCount);
            BEAST_EXPECT(ownerCountBefore == 2);  // Firewall + WithdrawPreauth

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Delete firewall
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify firewall deleted
            BEAST_EXPECT(!env.current()->exists(keylet::firewall(alice)));

            // Verify WithdrawPreauth for backup was also deleted
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));

            // Verify owner count decreased
            auto const ownerCountAfter = env.current()
                                             ->read(keylet::account(alice))
                                             ->getFieldU32(sfOwnerCount);
            BEAST_EXPECT(ownerCountAfter == 0);
        }

        // Delete with multiple WithdrawPreauth entries
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave, eve);
            env.close();

            // Create firewall
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Add additional WithdrawPreauth entries
            auto seq = env.seq(alice);
            auto fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            seq = env.seq(alice);
            fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, eve, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify all entries exist
            BEAST_EXPECT(env.current()->exists(keylet::firewall(alice)));
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, eve, 0)));

            auto const ownerCountBefore = env.current()
                                              ->read(keylet::account(alice))
                                              ->getFieldU32(sfOwnerCount);
            BEAST_EXPECT(
                ownerCountBefore == 4);  // 1 Firewall + 3 WithdrawPreauth

            // Delete firewall
            seq = env.seq(alice);
            fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify firewall and all WithdrawPreauth entries deleted
            BEAST_EXPECT(!env.current()->exists(keylet::firewall(alice)));
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));
            BEAST_EXPECT(!env.current()->exists(
                keylet::withdrawPreauth(alice, dave, 0)));
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, eve, 0)));

            // Verify owner count back to 0
            auto const ownerCountAfter = env.current()
                                             ->read(keylet::account(alice))
                                             ->getFieldU32(sfOwnerCount);
            BEAST_EXPECT(ownerCountAfter == 0);
        }

        // Delete after updating CounterParty
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // Create firewall
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Update CounterParty to dave
            auto seq = env.seq(alice);
            auto fee = firewall::calcFee(env, 1);
            auto jt = firewall::set(alice, firewallKey, seq, fee);
            jt[sfCounterParty.jsonName] = dave.human();
            env(jt, firewall::sig(carol), ter(tesSUCCESS));
            env.close();

            // Now dave is the counter party and must sign for delete
            seq = env.seq(alice);
            fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(dave),
                ter(tesSUCCESS));
            env.close();

            // Verify everything deleted
            BEAST_EXPECT(!env.current()->exists(keylet::firewall(alice)));
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));
        }

        // Verify that non-WithdrawPreauth entries in owner directory are not
        // deleted
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // Create some offers (which go in owner directory)
            env(offer(alice, alice["USD"](100), XRP(100)));
            env.close();

            auto const offersBefore = ownerDirCount(*env.current(), alice);
            BEAST_EXPECT(offersBefore == 1);

            // Create firewall
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto const itemsWithFirewall = ownerDirCount(*env.current(), alice);
            BEAST_EXPECT(
                itemsWithFirewall == 3);  // 1 offer + 1 firewall + 1 preauth

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Delete firewall
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(firewall::del(alice, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify offer still exists
            auto const ownerCountAfter = ownerDirCount(*env.current(), alice);
            BEAST_EXPECT(ownerCountAfter == 1);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testSetPreflightCreate(features);
        testSetPreflightUpdate(features);
        testSetPreclaimCreate(features);
        testSetPreclaimUpdate(features);
        testSetDoApplyCreate(features);
        testSetDoApplyUpdate(features);
        testMasterKeyDisable(features);
        testTransactionType(features);
        testDeletePreflight(features);
        testDeletePreclaim(features);
        testDeleteDoApply(features);
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
