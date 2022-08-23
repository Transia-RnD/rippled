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

#include <ripple/beast/unit_test/suite.hpp>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/XChainAttestations.h>

#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>
#include <test/jtx/multisign.h>
#include <test/jtx/xchain_bridge.h>

#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <fstream>
#include <iostream>

namespace ripple::test {

template <class T>
struct xEnv : public jtx::XChainBridgeObjects
{
    jtx::Env env_;

    xEnv(T& s, bool side = false)
        : env_(s, jtx::envconfig(jtx::port_increment, side ? 3 : 0), features)
    {
        using namespace jtx;
        STAmount xrp_funds{XRP(10000)};

        if (!side)
        {
            env_.fund(xrp_funds, mcDoor, mcAlice, mcBob, mcGw);

            // Signer's list must match the attestation signers
            // env_(jtx::signers(mcDoor, signers.size(), signers));
        }
        else
        {
            env_.fund(
                xrp_funds, scDoor, scAlice, scBob, scGw, scAttester, scReward);

            for (auto& ra : rewardAccounts)
                env_.fund(xrp_funds, ra);

            // Signer's list must match the attestation signers
            // env_(jtx::signers(Account::master, signers.size(), signers));
        }
    }

    xEnv&
    close()
    {
        env_.close();
        return *this;
    }

    template <class Arg, class... Args>
    xEnv&
    fund(STAmount const& amount, Arg const& arg, Args const&... args)
    {
        env_.fund(amount, arg, args...);
        return *this;
    }

    template <class JsonValue, class... FN>
    xEnv&
    tx(JsonValue&& jv, FN const&... fN)
    {
        env_(std::forward<JsonValue>(jv), fN...);
        return *this;
    }

    STAmount
    balance(jtx::Account const& account) const
    {
        return env_.balance(account);
    }
};

template <class T>
struct Balance
{
    T& env_;
    jtx::Account const& account_;
    STAmount startAmount;

    Balance(T& env, jtx::Account const& account) : account_(account), env_(env)
    {
        startAmount = env_.balance(account_).value();
    }

    STAmount
    diff() const
    {
        return env_.balance(account_).value() - startAmount;
    }
};

template <class T>
struct BalanceTransfer
{
    using balance = Balance<T>;

    balance from_;
    balance to_;
    std::vector<balance> reward_;

    BalanceTransfer(
        T& env,
        jtx::Account const& from_acct,
        jtx::Account const& to_acct,
        std::vector<jtx::Account> const& rewardAccounts)
        : from_(env, from_acct), to_(env, to_acct), reward_([&]() {
            std::vector<balance> r;
            r.reserve(rewardAccounts.size());
            for (auto& ra : rewardAccounts)
                r.emplace_back(env, ra);
            return r;
        }())
    {
    }

    bool
    has_happened(STAmount const& amt, STAmount const& reward)
    {
        return from_.diff() == -amt && to_.diff() == amt &&
            std::all_of(reward_.begin(), reward_.end(), [&](const balance& b) {
                   return b.diff() == reward;
               });
    }

    bool
    has_not_happened()
    {
        return has_happened(STAmount(0), STAmount(0));
    }
};

struct XChain_test : public beast::unit_test::suite,
                     public jtx::XChainBridgeObjects
{
    XRPAmount
    reserve(std::uint32_t count)
    {
        return xEnv(*this).env_.current()->fees().accountReserve(count);
    }

    void
    testBridgeCreate()
    {
        XRPAmount res1 = reserve(1);

        using namespace jtx;
        testcase("Bridge Create");

        // Bridge not owned by one of the door account.
        xEnv(*this).tx(create_bridge(mcBob), ter(temSIDECHAIN_NONDOOR_OWNER));

        // Create twice on the same account
        xEnv(*this)
            .tx(create_bridge(mcDoor))
            .close()
            .tx(create_bridge(mcDoor), ter(tecDUPLICATE));

        // Create USD bridge Alice -> Bob ... should succeed
        xEnv(*this).tx(
            create_bridge(
                mcAlice, bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"])),
            ter(tesSUCCESS));

        // Create where both door accounts are on the same chain. The second
        // bridge create should fail.
        xEnv(*this)
            .tx(create_bridge(
                mcAlice, bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"])))
            .close()
            .tx(create_bridge(
                    mcBob,
                    bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"])),
                ter(tecDUPLICATE));

        // Bridge where the two door accounts are equal.
        xEnv(*this).tx(
            create_bridge(
                mcBob, bridge(mcBob, mcBob["USD"], mcBob, mcBob["USD"])),
            ter(temEQUAL_DOOR_ACCOUNTS));

        // Create an bridge on an account with exactly enough balance to
        // meet the new reserve should succeed
        xEnv(*this)
            .fund(res1, mcuDoor)  // exact reserve for account + 1 object
            .close()
            .tx(create_bridge(mcuDoor, jvub), ter(tesSUCCESS));

        // Create an bridge on an account with no enough balance to meet the
        // new reserve
        xEnv(*this)
            .fund(res1 - 1, mcuDoor)  // just short of required reserve
            .close()
            .tx(create_bridge(mcuDoor, jvub), ter(tecINSUFFICIENT_RESERVE));

        // Reward amount is non-xrp
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, mcUSD(1)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is XRP and negative
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(-1)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is zero
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(0)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is 1 xrp => should succeed
        xEnv(*this).tx(create_bridge(mcDoor, jvb, XRP(1)), ter(tesSUCCESS));

        // Min create amount is 1 xrp, mincreate is 1 xrp => should succeed
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), XRP(1)), ter(tesSUCCESS));

        // Min create amount is non-xrp
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), mcUSD(100)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is zero (should fail, currently succeeds)
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), XRP(0)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is negative
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), XRP(-1)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));
    }

    void
    testBridgeCreateMatrix(bool markdown_output = true)
    {
        XRPAmount res1 = reserve(1);

        using namespace jtx;
        testcase("Bridge Create Matrix");

        // Test all combinations of the following:`
        // --------------------------------------
        // - Locking chain is IOU with locking chain door account as issuer
        // - Locking chain is IOU with issuing chain door account that
        // exists on
        //   the locking chain as issuer
        // - Locking chain is IOU with issuing chain door account that does
        // not
        //   exists on the locking chain as issuer
        // - Locking chain is IOU with non-door account (that exists on the
        //   locking chain ledger) as issuer
        // - Locking chain is IOU with non-door account (that does not exist
        //   exists on the locking chain ledger) as issuer
        // - Locking chain is XRP
        // ---------------------------------------------------------------------
        // - Issuing chain is IOU with issuing chain door account as the
        // issuer
        // - Issuing chain is IOU with locking chain door account (that
        // exists
        //   on the issuing chain ledger) as the issuer
        // - Issuing chain is IOU with locking chain door account (that does
        // not
        //   exist on the issuing chain ledger) as the issuer
        // - Issuing chain is IOU with non-door account (that exists on the
        //   issuing chain ledger) as the issuer
        // - Issuing chain is IOU with non-door account (that does not
        // exists on
        //   the issuing chain ledger) as the issuer
        // - Issuing chain is XRP and issuing chain door account is not the
        // root
        //   account
        // - Issuing chain is XRP and issuing chain door account is the root
        //   account
        // ---------------------------------------------------------------------
        // That's 42 combinations. The only combinations that should succeed
        // are: Locking chain is any IOU, Issuing chain is IOU with issuing
        // chain door account as the issuer Locking chain is XRP, Issuing
        // chain is XRP with issuing chain is the root account.
        // ---------------------------------------------------------------------
        Account a, b;
        Issue ia, ib;

        std::tuple lcs{
            std::make_pair(
                "Locking chain is IOU(locking chain door)",
                [&](auto& env) {
                    a = mcDoor;
                    ia = mcDoor["USD"];
                }),
            std::make_pair(
                "Locking chain is IOU(issuing chain door funded on locking "
                "chain)",
                [&](auto& env) {
                    a = mcDoor;
                    ia = scDoor["USD"];
                    env.fund(XRP(10000), scDoor);
                }),
            std::make_pair(
                "Locking chain is IOU(issuing chain door account unfunded "
                "on "
                "locking chain)",
                [&](auto& env) {
                    a = mcDoor;
                    ia = scDoor["USD"];
                }),
            std::make_pair(
                "Locking chain is IOU(bob funded on locking chain)",
                [&](auto& env) {
                    a = mcDoor;
                    ia = mcGw["USD"];
                }),
            std::make_pair(
                "Locking chain is IOU(bob unfunded on locking chain)",
                [&](auto& env) {
                    a = mcDoor;
                    ia = mcuGw["USD"];
                }),
            std::make_pair("Locking chain is XRP", [&](auto& env) {
                a = mcDoor;
                ia = xrpIssue();
            })};

        std::tuple ics{
            std::make_pair(
                "Issuing chain is IOU(issuing chain door account)",
                [&](auto& env) {
                    b = scDoor;
                    ib = scDoor["USD"];
                }),
            std::make_pair(
                "Issuing chain is IOU(locking chain door funded on issuing "
                "chain)",
                [&](auto& env) {
                    b = scDoor;
                    ib = mcDoor["USD"];
                    env.fund(XRP(10000), mcDoor);
                }),
            std::make_pair(
                "Issuing chain is IOU(locking chain door unfunded on "
                "issuing "
                "chain)",
                [&](auto& env) {
                    b = scDoor;
                    ib = mcDoor["USD"];
                }),
            std::make_pair(
                "Issuing chain is IOU(bob funded on issuing chain)",
                [&](auto& env) {
                    b = scDoor;
                    ib = mcGw["USD"];
                }),
            std::make_pair(
                "Issuing chain is IOU(bob unfunded on issuing chain)",
                [&](auto& env) {
                    b = scDoor;
                    ib = mcuGw["USD"];
                }),
            std::make_pair(
                "Issuing chain is XRP and issuing chain door account is "
                "not "
                "the root account",
                [&](auto& env) {
                    b = scDoor;
                    ib = xrpIssue();
                }),
            std::make_pair(
                "Issuing chain is XRP and issuing chain door account is "
                "the "
                "root account ",
                [&](auto& env) {
                    b = Account::master;
                    ib = xrpIssue();
                })};

        std::vector<std::tuple<TER, bool>> test_result;

        auto testcase = [&](auto const& lc, auto const& ic) {
            xEnv env(*this);
            lc.second(env);
            ic.second(env);
            env.tx(create_bridge(mcDoor, bridge(a, ia, b, ib)));
            test_result.emplace_back(env.env_.ter(), true);
        };

        auto apply_ics = [&](auto const& lc, auto const& ics) {
            std::apply(
                [&](auto const&... ic) { (testcase(lc, ic), ...); }, ics);
        };

        std::apply([&](auto const&... lc) { (apply_ics(lc, ics), ...); }, lcs);

        // optional output of matrix results in markdown format
        // ----------------------------------------------------
        if (!markdown_output)
            return;

        std::string fname{std::tmpnam(nullptr)};
        fname += ".md";
        std::cout << "Markdown output for matrix test: " << fname << "\n";

        std::ofstream(fname) << [&]() {
            size_t test_idx = 0;
            std::string res;
            res.reserve(10000);  // should be enough :-)

            // first two header lines
            res += "|  `issuing ->` | ";
            std::apply(
                [&](auto const&... ic) {
                    ((res += ic.first, res += " | "), ...);
                },
                ics);
            res += "\n";

            res += "| :--- | ";
            std::apply(
                [&](auto const&... ic) {
                    ((ic.first, res += ":---: |  "), ...);
                },
                ics);
            res += "\n";

            auto output = [&](auto const& lc, auto const& ic) {
                res += transToken(std::get<0>(test_result[test_idx]));
                res += " | ";
                ++test_idx;
            };

            auto output_ics = [&](auto const& lc, auto const& ics) {
                res += "| ";
                res += lc.first;
                res += " | ";
                std::apply(
                    [&](auto const&... ic) { (output(lc, ic), ...); }, ics);
                res += "\n";
            };

            std::apply(
                [&](auto const&... lc) { (output_ics(lc, ics), ...); }, lcs);

            return res;
        }();
    }

    void
    testBridgeModify()
    {
        XRPAmount res1 = reserve(1);

        using namespace jtx;
        testcase("Bridge Modify");

        // Changing a non-existent bridge should fail
        xEnv(*this).tx(
            bridge_modify(
                mcAlice,
                bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"]),
                XRP(2),
                XRP(10)),
            ter(tecNO_ENTRY));

        // must change something
        // xEnv(*this)
        //    .tx(create_bridge(mcDoor, jvb, XRP(1), XRP(1)))
        //    .tx(bridge_modify(mcDoor, jvb, XRP(1), XRP(1)),
        //    ter(temMALFORMED));

        // must change something
        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb, XRP(1), XRP(1)))
            .close()
            .tx(bridge_modify(mcDoor, jvb, {}, {}), ter(temMALFORMED));

        // Reward amount is non-xrp
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, mcUSD(2), XRP(10)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is XRP and negative
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(-2), XRP(10)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is zero
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(0), XRP(10)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Min create amount is non-xrp
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(2), mcUSD(10)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is zero
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(2), XRP(0)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is negative
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(2), XRP(-10)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // First check the regular claim process (without bridge_modify)
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, rewardAccounts);

            Json::Value batch = attestation_claim_batch(
                jvb, mcAlice, amt, rewardAccounts, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAlice, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }
    }

    void
    testBridgeClaim()
    {
        XRPAmount res1 = reserve(1);

        using namespace jtx;
        testcase("Bridge Claim");

        // Claim against non-existent bridge

        // Claim against non-existent claim id

        // Claim against a claim id owned by another account

        // Claim against a claim id with no attestations

        // Claim against a claim id with attestations, but not enough to make a
        // quorum

        // Claim id of zero

        // Claim issue that does not match the expected issue on the bridge
        // (either LockingChainIssue or IssuingChainIssue, depending on the
        // chain). The claim id should already have enough attestations to reach
        // a quorum for this amount (for a different issuer).

        // Claim to a destination that does not already exist on the chain

        // Claim where the claim id owner does not have enough XRP to pay the
        // reward

        // Claim where the claim id owner has enough XRP to pay the reward, but
        // it would put his balance below the reserve

        // Pay to an account with deposit auth set

        // Claim where the amount different from what is attested to

        // Verify that rewards are paid from the account that owns the claim id

        // Verify that if a reward is not evenly divisible amung the reward
        // accounts, the remaining amount goes to the claim id owner.

        // If a reward distribution fails for one of the reward accounts (the
        // reward account doesn't exist or has deposit auth set), then the txn
        // should still succeed, but that portion should go to the claim id
        // owner.

        // Verify that if a batch of attestations brings the signatures over
        // quorum (say the quorum is 4 and there are 5 attestations) then the
        // reward should be split amung the five accounts.
    }

    void
    run() override
    {
        testBridgeCreate();
        // testBridgeCreateMatrix();
        testBridgeModify();
        testBridgeClaim();
    }
};

BEAST_DEFINE_TESTSUITE(XChain, app, ripple);

}  // namespace ripple::test
