#pragma once

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/STAmount.h>

namespace xrpl::test::jtx {

/**
 * Subscription operations.
 */
namespace subscription {

json::Value
create(
    jtx::Account const& account,
    jtx::Account const& destination,
    STAmount const& amount,
    NetClock::duration const& frequency,
    std::optional<NetClock::time_point> const& expiration = std::nullopt);

json::Value
update(
    jtx::Account const& account,
    uint256 const& subscriptionId,
    STAmount const& amount,
    std::optional<NetClock::time_point> const& expiration = std::nullopt);

json::Value
cancel(jtx::Account const& account, uint256 const& subscriptionId);

json::Value
claim(jtx::Account const& account, uint256 const& subscriptionId, STAmount const& amount);

/**
 * Set the "StartTime" time tag on a JTx
 */
class start_time
{
private:
    NetClock::time_point value_;

public:
    explicit start_time(NetClock::time_point const& value) : value_(value)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

}  // namespace subscription

}  // namespace xrpl::test::jtx
