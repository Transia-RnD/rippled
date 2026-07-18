#include <test/jtx/Account.h>
#include <test/jtx/Env.h>
#include <test/jtx/amount.h>
#include <test/jtx/balance.h>
#include <test/jtx/fee.h>
#include <test/jtx/flags.h>
#include <test/jtx/pay.h>
#include <test/jtx/ter.h>
#include <test/jtx/trust.h>
#include <test/jtx/utility.h>

#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/jss.h>

#include <chrono>

namespace xrpl {

class Coupon_test : public beast::unit_test::Suite
{
    FeatureBitset const all_{test::jtx::testableAmendments()};

    static json::Value
    scheduleCreate(
        test::jtx::Account const& issuer,
        Asset const& bondAsset,
        STAmount const& couponAmount,
        std::uint32_t interval,
        std::uint32_t firstCouponTime)
    {
        json::Value jv;
        jv[jss::TransactionType] = jss::CouponScheduleCreate;
        jv[jss::Account] = issuer.human();
        jv[sfBondAsset] = toJson(bondAsset);
        jv[sfCouponAmount] = toJson(couponAmount);
        jv[sfCouponInterval] = interval;
        jv[sfFirstCouponTime] = firstCouponTime;
        return jv;
    }

    static json::Value
    scheduleSet(test::jtx::Account const& issuer, uint256 const& id, std::uint32_t expiration)
    {
        json::Value jv;
        jv[jss::TransactionType] = jss::CouponScheduleSet;
        jv[jss::Account] = issuer.human();
        jv[sfCouponScheduleID] = to_string(id);
        jv[sfExpiration] = expiration;
        return jv;
    }

    static json::Value
    scheduleDelete(test::jtx::Account const& issuer, uint256 const& id)
    {
        json::Value jv;
        jv[jss::TransactionType] = jss::CouponScheduleDelete;
        jv[jss::Account] = issuer.human();
        jv[sfCouponScheduleID] = to_string(id);
        return jv;
    }

    static json::Value
    couponRegister(test::jtx::Account const& holder, uint256 const& id, STAmount const& units)
    {
        json::Value jv;
        jv[jss::TransactionType] = jss::CouponRegister;
        jv[jss::Account] = holder.human();
        jv[sfCouponScheduleID] = to_string(id);
        jv[jss::Amount] = toJson(units);
        return jv;
    }

    static json::Value
    couponUnregister(
        test::jtx::Account const& holder,
        uint256 const& id,
        std::optional<STAmount> const& units = std::nullopt)
    {
        json::Value jv;
        jv[jss::TransactionType] = jss::CouponUnregister;
        jv[jss::Account] = holder.human();
        jv[sfCouponScheduleID] = to_string(id);
        if (units)
            jv[jss::Amount] = toJson(*units);
        return jv;
    }

    static json::Value
    couponClaim(
        test::jtx::Account const& holder,
        uint256 const& id,
        std::optional<STAmount> const& amount = std::nullopt)
    {
        json::Value jv;
        jv[jss::TransactionType] = jss::CouponClaim;
        jv[jss::Account] = holder.human();
        jv[sfCouponScheduleID] = to_string(id);
        if (amount)
            jv[jss::Amount] = toJson(*amount);
        return jv;
    }

    static std::uint32_t
    nowSeconds(test::jtx::Env& env)
    {
        return env.now().time_since_epoch().count();
    }

    void
    testDisabled(FeatureBitset features)
    {
        testcase("disabled");
        using namespace test::jtx;

        Env env(*this, features - featureCouponPayments);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        env.fund(XRP(10'000), issuer, gw);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];
        env(scheduleCreate(issuer, BND.issue(), USD(1), 1'000, nowSeconds(env) + 100),
            Ter(temDISABLED));
    }

    void
    testScheduleCreate(FeatureBitset features)
    {
        testcase("schedule create");
        using namespace test::jtx;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        env.fund(XRP(10'000), issuer, gw);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];
        std::uint32_t const future = nowSeconds(env) + 1'000;

        // Bond asset cannot be XRP.
        {
            auto jv = scheduleCreate(issuer, xrpIssue(), USD(1), 1'000, future);
            env(jv, Ter(temMALFORMED));
        }
        // Coupon amount must be positive.
        env(scheduleCreate(issuer, BND.issue(), USD(0), 1'000, future), Ter(temBAD_AMOUNT));
        // A bond paying coupons in itself is rebasing, not interest.
        env(scheduleCreate(issuer, BND.issue(), BND(1), 1'000, future), Ter(temMALFORMED));
        // Interval must be > 0.
        env(scheduleCreate(issuer, BND.issue(), USD(1), 0, future), Ter(temMALFORMED));
        // Expiration must be after FirstCouponTime.
        {
            auto jv = scheduleCreate(issuer, BND.issue(), USD(1), 1'000, future);
            jv[sfExpiration] = future;
            env(jv, Ter(temBAD_EXPIRATION));
        }
        // CallNoticePeriod must be > 0 when present.
        {
            auto jv = scheduleCreate(issuer, BND.issue(), USD(1), 1'000, future);
            jv[sfCallNoticePeriod] = 0;
            env(jv, Ter(temMALFORMED));
        }
        // EarliestCallTime requires CallNoticePeriod.
        {
            auto jv = scheduleCreate(issuer, BND.issue(), USD(1), 1'000, future);
            jv[sfEarliestCallTime] = future + 10;
            env(jv, Ter(temMALFORMED));
        }
        // FirstCouponTime must be in the future. (The jtx clock starts
        // near the NetClock epoch, so 1 is always in the past.)
        env(scheduleCreate(issuer, BND.issue(), USD(1), 1'000, 1), Ter(tecEXPIRED));

        // Success.
        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        env(scheduleCreate(issuer, BND.issue(), USD(2.5), 1'000, future));
        env.close();
        {
            auto const sle = env.le(k);
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT((*sle)[sfAccount] == issuer.id());
            BEAST_EXPECT((*sle)[sfCouponInterval] == 1'000);
            BEAST_EXPECT((*sle)[sfFirstCouponTime] == future);
            BEAST_EXPECT((*sle)[sfRegistrationCount] == 0);
        }
        BEAST_EXPECT(env.ownerCount(issuer) == 1);

        // One schedule per (Account, BondAsset).
        env(scheduleCreate(issuer, BND.issue(), USD(2.5), 1'000, nowSeconds(env) + 500),
            Ter(tecDUPLICATE));

        // A different bond asset gets its own schedule.
        auto const BD2 = issuer["BD2"];
        env(scheduleCreate(issuer, BD2.issue(), USD(1), 1'000, nowSeconds(env) + 500));
        env.close();
        BEAST_EXPECT(env.ownerCount(issuer) == 2);
    }

    void
    testRegister(FeatureBitset features)
    {
        testcase("register");
        using namespace test::jtx;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        Account const alice{"alice"};
        env.fund(XRP(10'000), issuer, gw, alice);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];
        std::uint32_t const future = nowSeconds(env) + 1'000;

        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        env(scheduleCreate(issuer, BND.issue(), USD(2.5), 1'000, future));
        env.close();

        // No such schedule.
        env(couponRegister(alice, keylet::couponSchedule(gw.id(), BND.issue()).key, BND(10)),
            Ter(tecNO_ENTRY));

        // Wrong asset.
        env(couponRegister(alice, k.key, USD(10)), Ter(tecWRONG_ASSET));

        // Bond issuer has not opted into trust-line locking.
        env.trust(BND(1'000), alice);
        env(pay(issuer, alice, BND(100)));
        env.close();
        env(couponRegister(alice, k.key, BND(10)), Ter(tecNO_PERMISSION));
        // Seal the tec into a closed ledger so the open-ledger retry
        // cannot succeed once the flag below is set.
        env.close();

        env(fset(issuer, asfAllowTrustLineLocking));
        env.close();

        // More than the spendable balance.
        env(couponRegister(alice, k.key, BND(500)), Ter(tecINSUFFICIENT_FUNDS));

        // Success: units are locked, registration exists.
        env(couponRegister(alice, k.key, BND(100)));
        env.close();

        auto const regKeylet = keylet::couponRegistration(k.key, alice.id());
        {
            auto const reg = env.le(regKeylet);
            if (!BEAST_EXPECT(reg))
                return;
            BEAST_EXPECT((*reg)[sfRegisteredUnits] == BND(100));
            BEAST_EXPECT((*reg)[sfAccruedAmount] == USD(0));
        }
        env.require(Balance(alice, BND(0)));
        {
            auto const sle = env.le(k);
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT((*sle)[sfRegistrationCount] == 1);
        }

        // Registered units cannot be spent.
        env(pay(alice, issuer, BND(10)), Ter(tecPATH_DRY));
    }

    void
    testAccrualAndClaim(FeatureBitset features)
    {
        testcase("accrual and claim");
        using namespace test::jtx;
        using namespace std::chrono_literals;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10'000), issuer, gw, alice, bob);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];

        env(fset(issuer, asfAllowTrustLineLocking));
        env.trust(BND(10'000), alice, bob);
        env.trust(USD(1'000'000), issuer);
        env(pay(issuer, alice, BND(100)));
        env(pay(issuer, bob, BND(100)));
        env(pay(gw, issuer, USD(10'000)));
        env.close();

        std::uint32_t const interval = 1'000;
        std::uint32_t const first = nowSeconds(env) + 100;
        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        env(scheduleCreate(issuer, BND.issue(), USD(2.5), interval, first));
        env.close();

        // Alice registers before the first coupon date.
        env(couponRegister(alice, k.key, BND(100)));
        env.close();

        // Nothing accrued yet.
        env(couponClaim(alice, k.key), Ter(tecNO_PERMISSION));
        env.close();

        // Cross the first coupon date: 100 units * 2.5 USD = 250 USD.
        env.close(env.now() + 200s);
        env(couponClaim(alice, k.key));
        env.close();
        env.require(Balance(alice, USD(250)));

        // Bob registers after the first date: he earns nothing for it.
        env(couponRegister(bob, k.key, BND(100)));
        env.close();
        env(couponClaim(bob, k.key), Ter(tecNO_PERMISSION));
        env.close();

        // Cross two more dates: arrears accumulate and settle in one claim.
        env.close(env.now() + std::chrono::seconds(2 * interval));
        env(couponClaim(alice, k.key));
        env.close();
        env.require(Balance(alice, USD(750)));

        // Bob was registered for those two dates.
        env(couponClaim(bob, k.key));
        env.close();
        env.require(Balance(bob, USD(500)));

        // Partial claim.
        env.close(env.now() + std::chrono::seconds(interval));
        env(couponClaim(alice, k.key, USD(100)));
        env.close();
        env.require(Balance(alice, USD(850)));
        {
            auto const reg = env.le(keylet::couponRegistration(k.key, alice.id()));
            if (!BEAST_EXPECT(reg))
                return;
            BEAST_EXPECT((*reg)[sfAccruedAmount] == USD(150));
        }

        // Unregister does not pay accrued coupons, and the claim to
        // arrears survives exiting the position.
        env(couponUnregister(alice, k.key));
        env.close();
        env.require(Balance(alice, BND(100)));
        {
            auto const reg = env.le(keylet::couponRegistration(k.key, alice.id()));
            if (!BEAST_EXPECT(reg))
                return;
            BEAST_EXPECT((*reg)[sfRegisteredUnits] == BND(0));
            BEAST_EXPECT((*reg)[sfAccruedAmount] == USD(150));
        }
        env(couponClaim(alice, k.key));
        env.close();
        env.require(Balance(alice, USD(1'000)));
        // Registration auto-deleted at zero/zero.
        BEAST_EXPECT(!env.le(keylet::couponRegistration(k.key, alice.id())));
    }

    void
    testUnderfundedIssuer(FeatureBitset features)
    {
        testcase("underfunded issuer");
        using namespace test::jtx;
        using namespace std::chrono_literals;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        Account const alice{"alice"};
        env.fund(XRP(10'000), issuer, gw, alice);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];

        env(fset(issuer, asfAllowTrustLineLocking));
        env.trust(BND(10'000), alice);
        env.trust(USD(1'000'000), issuer);
        env(pay(issuer, alice, BND(100)));
        env.close();

        std::uint32_t const first = nowSeconds(env) + 100;
        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        env(scheduleCreate(issuer, BND.issue(), USD(2.5), 1'000, first));
        env.close();
        env(couponRegister(alice, k.key, BND(100)));
        env.close();

        env.close(env.now() + 200s);

        // The issuer has no USD: the dishonored coupon is a visible,
        // timestamped failed claim — and the accrual survives it.
        env(couponClaim(alice, k.key), Ter(tecINSUFFICIENT_FUNDS));
        env.close();

        // The issuer funds up; the retry succeeds.
        env(pay(gw, issuer, USD(10'000)));
        env.close();
        env(couponClaim(alice, k.key));
        env.close();
        env.require(Balance(alice, USD(250)));
    }

    void
    testCallable(FeatureBitset features)
    {
        testcase("callable");
        using namespace test::jtx;
        using namespace std::chrono_literals;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        Account const alice{"alice"};
        env.fund(XRP(10'000), issuer, gw, alice);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];

        env(fset(issuer, asfAllowTrustLineLocking));
        env.trust(BND(10'000), alice);
        env.trust(USD(1'000'000), issuer);
        env(pay(issuer, alice, BND(100)));
        env(pay(gw, issuer, USD(10'000)));
        env.close();

        std::uint32_t const interval = 1'000;

        // A non-callable schedule is fully immutable.
        {
            auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
            env(scheduleCreate(issuer, BND.issue(), USD(2.5), interval, nowSeconds(env) + 100));
            env.close();
            env(scheduleSet(issuer, k.key, nowSeconds(env) + 10'000), Ter(tecNO_PERMISSION));
            env(scheduleDelete(issuer, k.key));
            env.close();
        }

        // Callable: notice floor and shorten-only are enforced; accrual
        // stops at the called Expiration.
        std::uint32_t const notice = 2'000;
        std::uint32_t const first = nowSeconds(env) + 100;
        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        {
            auto jv = scheduleCreate(issuer, BND.issue(), USD(2.5), interval, first);
            jv[sfCallNoticePeriod] = notice;
            env(jv);
            env.close();
        }
        env(couponRegister(alice, k.key, BND(100)));
        env.close();

        // Only the issuer can call.
        env(scheduleSet(alice, k.key, nowSeconds(env) + notice + 100), Ter(tecNO_PERMISSION));
        // The notice floor.
        env(scheduleSet(issuer, k.key, nowSeconds(env) + notice - 500), Ter(tecNO_PERMISSION));

        // A valid call.
        std::uint32_t const callTime = nowSeconds(env) + notice + 500;
        env(scheduleSet(issuer, k.key, callTime));
        env.close();
        {
            auto const sle = env.le(k);
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT((*sle)[~sfExpiration] == callTime);
        }

        // Shorten-only: the call cannot be extended.
        env(scheduleSet(issuer, k.key, callTime + 10'000), Ter(tecNO_PERMISSION));

        // Cross well past the called expiration: only the dates strictly
        // before Expiration accrue.
        env.close(env.now() + std::chrono::seconds(6 * interval));
        env(couponClaim(alice, k.key));
        env.close();
        // Dates: first, first+1000, first+2000 (all < callTime ≈ first+2400).
        env.require(Balance(alice, USD(750)));

        // Nothing further ever accrues.
        env.close(env.now() + std::chrono::seconds(2 * interval));
        env(couponClaim(alice, k.key), Ter(tecNO_PERMISSION));
    }

    void
    testScheduleDelete(FeatureBitset features)
    {
        testcase("schedule delete");
        using namespace test::jtx;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const gw{"gw"};
        Account const alice{"alice"};
        env.fund(XRP(10'000), issuer, gw, alice);
        env.close();

        auto const BND = issuer["BND"];
        auto const USD = gw["USD"];

        env(fset(issuer, asfAllowTrustLineLocking));
        env.trust(BND(10'000), alice);
        env(pay(issuer, alice, BND(100)));
        env.close();

        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        env(scheduleCreate(issuer, BND.issue(), USD(2.5), 1'000, nowSeconds(env) + 100));
        env.close();

        env(scheduleDelete(alice, k.key), Ter(tecNO_PERMISSION));

        env(couponRegister(alice, k.key, BND(50)));
        env.close();

        // A schedule with registrants cannot be deleted.
        env(scheduleDelete(issuer, k.key), Ter(tecHAS_OBLIGATIONS));
        // Seal the tec so its open-ledger retry cannot delete the
        // schedule after the unregister below.
        env.close();

        env(couponUnregister(alice, k.key));
        env.close();

        env(scheduleDelete(issuer, k.key));
        env.close();
        BEAST_EXPECT(!env.le(k));
        BEAST_EXPECT(env.ownerCount(issuer) == 0);
    }

    void
    testXRPCoupon(FeatureBitset features)
    {
        testcase("XRP coupon asset");
        using namespace test::jtx;
        using namespace std::chrono_literals;

        Env env(*this, features);
        Account const issuer{"issuer"};
        Account const alice{"alice"};
        env.fund(XRP(10'000), issuer, alice);
        env.close();

        auto const BND = issuer["BND"];

        env(fset(issuer, asfAllowTrustLineLocking));
        env.trust(BND(10'000), alice);
        env(pay(issuer, alice, BND(100)));
        env.close();

        auto const k = keylet::couponSchedule(issuer.id(), BND.issue());
        env(scheduleCreate(issuer, BND.issue(), XRP(1), 1'000, nowSeconds(env) + 100));
        env.close();
        env(couponRegister(alice, k.key, BND(100)));
        env.close();

        auto const before = env.balance(alice);
        env.close(env.now() + 200s);
        env(couponClaim(alice, k.key), Fee(drops(10)));
        env.close();
        // 100 units * 1 XRP - the claim fee.
        BEAST_EXPECT(env.balance(alice) == before + XRP(100) - drops(10));
    }

public:
    void
    run() override
    {
        testDisabled(all_);
        testScheduleCreate(all_);
        testRegister(all_);
        testAccrualAndClaim(all_);
        testUnderfundedIssuer(all_);
        testCallable(all_);
        testScheduleDelete(all_);
        testXRPCoupon(all_);
    }
};

BEAST_DEFINE_TESTSUITE(Coupon, app, xrpl);

}  // namespace xrpl
