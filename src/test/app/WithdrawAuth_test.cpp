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

#include <xrpl/protocol/Feature.h>

namespace ripple {
namespace test {

struct WithdrawAuth_test : public beast::unit_test::suite
{
    void
    testEnable(FeatureBitset features)
    {
        testcase("withdraw preauth enable");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // Test with feature disabled
        {
            auto const amend = features - featureFirewall;
            Env env{*this, amend};
            env.fund(XRP(1000), alice, bob);
            env.close();

            // Cannot create WithdrawPreauth when feature disabled
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, bob, uint256{1}, seq, fee),
                ter(temDISABLED));

            // Cannot remove WithdrawPreauth when feature disabled
            env(withdraw::unauth(alice, bob, uint256{1}, seq, fee),
                ter(temDISABLED));
        }

        // Test with feature enabled
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, dave);
            env.close();

            // First create a firewall for alice
            env(firewall::set(alice),
                firewall::backup(bob),
                firewall::counter_party(carol),
                ter(tesSUCCESS));
            env.close();

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Now can add WithdrawPreauth
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify the WithdrawPreauth entry exists
            auto const slePreauth =
                env.current()->read(keylet::withdrawPreauth(alice, dave, 0));
            BEAST_EXPECT(slePreauth);
            BEAST_EXPECT(slePreauth->getAccountID(sfAccount) == alice.id());
            BEAST_EXPECT(slePreauth->getAccountID(sfAuthorize) == dave.id());

            // Remove the WithdrawPreauth
            auto const seq1 = env.seq(alice);
            auto const fee1 = firewall::calcFee(env, 1);
            env(withdraw::unauth(alice, dave, firewallKey, seq1, fee1),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify it's gone
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));
        }
    }

    void
    testPreflight(FeatureBitset features)
    {
        testcase("withdraw preauth preflight");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        // Create firewall first
        env(firewall::set(alice),
            firewall::backup(bob),
            firewall::counter_party(carol),
            ter(tesSUCCESS));
        env.close();

        auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

        // Invalid flags
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, bob, firewallKey, seq, fee),
                txflags(tfSell),
                firewall::sig(carol),
                ter(temINVALID_FLAG));
        }

        // Neither Authorize nor Unauthorize present
        // TODO: Fix
        // {
        //     env(withdraw::preauth(alice, bob, firewallKey),
        //     firewall::sig(carol), ter(temMALFORMED));
        // }

        // Both Authorize and Unauthorize present
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = withdraw::auth(alice, bob, firewallKey, seq, fee);
            jt[sfUnauthorize.jsonName] = carol.human();
            env(jt, firewall::sig(carol), ter(temMALFORMED));
        }

        // // Zero account in Authorize field
        // {
        //     auto jt = withdraw::auth(alice, xrpAccount(), firewallKey);
        //     env(jt, firewall::sig(carol), ter(temINVALID_ACCOUNT_ID));
        // }

        // // Zero account in Unauthorize field
        // {
        //     auto jt = withdraw::unauth(alice, xrpAccount(), firewallKey);
        //     env(jt, firewall::sig(carol), ter(temINVALID_ACCOUNT_ID));
        // }

        // Cannot preauthorize self
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, alice, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(temCANNOT_PREAUTH_SELF));
        }

        // Missing FirewallID
        // TODO: Fix: Delete the FirewallID field
        // {
        //     auto jt = withdraw::preauth(alice);
        //     jt[sfAuthorize.jsonName] = bob.human();
        //     // No FirewallID field
        //     env(jt, ter(temMALFORMED));
        // }

        // Missing FirewallSigners
        {
            // No FirewallSigners
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, carol, firewallKey, seq, fee),
                ter(temMALFORMED));
        }

        // Bad signature
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            auto jt = withdraw::auth(alice, bob, firewallKey, seq, fee);
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
    testPreclaim(FeatureBitset features)
    {
        testcase("withdraw preauth preclaim");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");
        Account const elsa = Account("elsa");

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        // Firewall does not exist
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, bob, uint256{1}, seq, fee),
                firewall::sig(carol),
                ter(tecNO_TARGET));
            env.close();
        }

        // Create firewall
        env(firewall::set(alice),
            firewall::backup(bob),
            firewall::counter_party(carol),
            ter(tesSUCCESS));
        env.close();

        auto [firewallKey, firewallSle] =
            firewall::keyAndSle(*env.current(), alice);

        // Target account does not exist (for authorize)
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env.memoize(dave);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tecNO_TARGET));
            env.close();
        }

        // Fund dave now
        env.fund(XRP(1000), dave);
        env.close();

        // Duplicate preauth entry
        {
            // First create one for bob (already exists from firewall creation)
            // Try to create duplicate
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, bob, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tecDUPLICATE));
            env.close();
        }

        // Try to remove non-existent entry
        {
            env.memoize(elsa);
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::unauth(alice, elsa, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tecNO_ENTRY));
            env.close();
        }

        // Wrong signer (not the counter party)
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(bob),  // bob is backup, not counter party
                ter(tefBAD_AUTH));
            env.close();
        }

        // Successful authorize
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify it exists
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));
        }

        // Successful unauthorize
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::unauth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify it's gone
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));
        }
    }

    void
    testDoApply(FeatureBitset features)
    {
        testcase("withdraw preauth doapply");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");

        // Test successful creation
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

            // Check owner count before
            auto const sleOwnerBefore =
                env.current()->read(keylet::account(alice));
            auto const ownerCountBefore =
                sleOwnerBefore->getFieldU32(sfOwnerCount);

            // Add preauth for dave
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify SLE created correctly
            auto const slePreauth =
                env.current()->read(keylet::withdrawPreauth(alice, dave, 0));
            BEAST_EXPECT(slePreauth);
            BEAST_EXPECT(slePreauth->getAccountID(sfAccount) == alice.id());
            BEAST_EXPECT(slePreauth->getAccountID(sfAuthorize) == dave.id());
            BEAST_EXPECT(slePreauth->isFieldPresent(sfOwnerNode));
            BEAST_EXPECT(slePreauth->isFieldPresent(sfPreviousTxnID));
            BEAST_EXPECT(slePreauth->isFieldPresent(sfPreviousTxnLgrSeq));

            // Verify owner count increased
            auto const sleOwnerAfter =
                env.current()->read(keylet::account(alice));
            BEAST_EXPECT(
                sleOwnerAfter->getFieldU32(sfOwnerCount) ==
                ownerCountBefore + 1);
        }

        // // Test insufficient reserve
        // {
        //     Env env{*this, features};
        //     env.fund(XRP(1000), alice, bob, carol, dave);
        //     env.close();

        //     // Create firewall
        //     env(firewall::set(alice),
        //         firewall::backup(bob),
        //         firewall::counter_party(carol),
        //         ter(tesSUCCESS));
        //     env.close();

        //     auto [firewallKey, _] = firewall::keyAndSle(*env.current(),
        //     alice);

        //     // Drain alice's balance to near reserve
        //     auto const reserve = env.current()->fees().accountReserve(
        //         2);  // Already has 2 objects
        //     auto const baseFee = env.current()->fees().base;
        //     env(
        //         pay(alice,
        //             bob,
        //             alice.balance(*env.current()) - reserve - baseFee * 2));
        //     env.close();

        //     // Try to add preauth with insufficient reserve
        //     env(withdraw::auth(alice, dave, firewallKey),
        //         firewall::sig(carol),
        //         ter(tecINSUFFICIENT_RESERVE));
        // }

        // Test directory full
        // {
        //     Env env{*this, features};
        //     env.fund(XRP(100000), alice, bob, carol, dave);
        //     env.close();

        //     // Create firewall
        //     env(firewall::set(alice),
        //         firewall::backup(bob),
        //         firewall::counter_party(carol),
        //         ter(tesSUCCESS));
        //     env.close();

        //     auto [firewallKey, _] = firewall::keyAndSle(*env.current(),
        //     alice);

        //     // Fill owner directory (minus the 2 entries already there)
        //     for (int i = 0; i < 30; ++i)
        //     {
        //         env(offer(alice, XRP(1), alice["USD"](1)));
        //         env.close();
        //     }

        //     // Try to add preauth when directory is full
        //     auto const seq = env.seq(alice);
        //     auto const fee = firewall::calcFee(env, 1);
        //     env(withdraw::auth(alice, dave, firewallKey, seq, fee),
        //         firewall::sig(carol),
        //         ter(tecDIR_FULL));
        // }

        // Test successful removal
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

            // Add preauth for dave
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Check owner count before removal
            auto const sleOwnerBefore =
                env.current()->read(keylet::account(alice));
            auto const ownerCountBefore =
                sleOwnerBefore->getFieldU32(sfOwnerCount);

            // Remove preauth
            auto const seq1 = env.seq(alice);
            auto const fee1 = firewall::calcFee(env, 1);
            env(withdraw::unauth(alice, dave, firewallKey, seq1, fee1),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify it's gone
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));

            // Verify owner count decreased
            auto const sleOwnerAfter =
                env.current()->read(keylet::account(alice));
            BEAST_EXPECT(
                sleOwnerAfter->getFieldU32(sfOwnerCount) ==
                ownerCountBefore - 1);
        }

        // Test removal of initial backup preauth
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

            auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

            // Verify bob has preauth (from being backup)
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));

            // Remove bob's preauth
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::unauth(alice, bob, firewallKey, seq, fee),
                firewall::sig(carol),
                ter(tesSUCCESS));
            env.close();

            // Verify it's gone
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, bob, 0)));
        }
    }

    void
    testMultiSig(FeatureBitset features)
    {
        testcase("withdraw preauth multisig");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");
        Account const eve = Account("eve");

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol, dave, eve);
        env.close();

        // Set up multisig for carol (the counter party)
        env(signers(carol, 2, {{dave, 1}, {eve, 1}}));
        env.close();

        // Create firewall with carol as counter party
        env(firewall::set(alice),
            firewall::backup(bob),
            firewall::counter_party(carol),
            ter(tesSUCCESS));
        env.close();

        auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

        // Add preauth using multisig from carol's signers
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::auth(alice, dave, firewallKey, seq, fee),
                firewall::msig(carol, dave, eve),
                ter(tesSUCCESS));
            env.close();

            // Verify it was created
            BEAST_EXPECT(
                env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));
        }

        // Remove preauth using multisig
        {
            auto const seq = env.seq(alice);
            auto const fee = firewall::calcFee(env, 1);
            env(withdraw::unauth(alice, dave, firewallKey, seq, fee),
                firewall::msig(carol, dave, eve),
                ter(tesSUCCESS));
            env.close();

            // Verify it was removed
            BEAST_EXPECT(
                !env.current()->exists(keylet::withdrawPreauth(alice, dave, 0)));
        }
    }

    void
    testIntegration(FeatureBitset features)
    {
        testcase("withdraw preauth integration");
        using namespace jtx;

        Account const alice = Account("alice");
        Account const bob = Account("bob");
        Account const carol = Account("carol");
        Account const dave = Account("dave");
        Account const eve = Account("eve");

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol, dave, eve);
        env.close();

        // Create firewall for alice
        env(firewall::set(alice),
            firewall::backup(bob),
            firewall::counter_party(carol),
            ter(tesSUCCESS));
        env.close();

        auto [firewallKey, _] = firewall::keyAndSle(*env.current(), alice);

        // Initially bob can pay alice (he's the backup)
        env(pay(bob, alice, XRP(10)), ter(tesSUCCESS));
        env.close();

        // Alice cannot pay dave (no preauth)
        env(pay(alice, dave, XRP(10)), ter(tecFIREWALL_BLOCK));
        env.close();

        // Add preauth for dave
        auto const seq = env.seq(alice);
        auto const fee = firewall::calcFee(env, 1);
        env(withdraw::auth(alice, dave, firewallKey, seq, fee),
            firewall::sig(carol),
            ter(tesSUCCESS));
        env.close();

        // Now alice can pay dave
        env(pay(alice, dave, XRP(10)), ter(tesSUCCESS));
        env.close();

        // Alice still cannot pay eve
        env(pay(alice, eve, XRP(10)), ter(tecFIREWALL_BLOCK));
        env.close();

        // Remove dave's preauth
        auto const seq1 = env.seq(alice);
        auto const fee1 = firewall::calcFee(env, 1);
        env(withdraw::unauth(alice, dave, firewallKey, seq1, fee1),
            firewall::sig(carol),
            ter(tesSUCCESS));
        env.close();

        // Alice can no longer pay dave
        env(pay(alice, dave, XRP(10)), ter(tecFIREWALL_BLOCK));
        env.close();

        // Update firewall counter party to dave
        auto const seq2 = env.seq(alice);
        auto const fee2 = firewall::calcFee(env, 1);
        auto jt = firewall::set(alice, firewallKey, seq2, fee2);
        jt[sfCounterParty.jsonName] = dave.human();
        env(jt, firewall::sig(carol), ter(tesSUCCESS));
        env.close();

        // Now dave is the counter party and can authorize
        auto const seq3 = env.seq(alice);
        auto const fee3 = firewall::calcFee(env, 1);
        env(withdraw::auth(alice, eve, firewallKey, seq3, fee3),
            firewall::sig(dave),
            ter(tesSUCCESS));
        env.close();

        // Alice can now pay eve
        env(pay(alice, eve, XRP(10)), ter(tesSUCCESS));
        env.close();
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnable(features);
        testPreflight(features);
        testPreclaim(features);
        testDoApply(features);
        // testMultiSig(features);
        testIntegration(features);
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{testable_amendments()};
        testWithFeats(all);
    }
};

BEAST_DEFINE_TESTSUITE(WithdrawAuth, app, ripple);

}  // namespace test
}  // namespace ripple
