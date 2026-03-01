#pragma once

#include <xrpl/basics/CountedObject.h>
#include <xrpl/protocol/Issue.h>

#include <boost/utility/base_from_member.hpp>

namespace xrpl {

class Option final : public CountedObject<Option>
{
public:
    Issue issue;
    uint64_t strike;
    uint32_t expiration;

    Option()
    {
    }

    Option(Issue issue_, uint64_t strike_, uint32_t expiration_)
        : issue(issue_), strike(strike_), expiration(expiration_)
    {
    }
};

std::string
to_string(Option const& option);

std::ostream&
operator<<(std::ostream& os, Option const& x);

template <class Hasher>
void
hash_append(Hasher& h, Option const& o)
{
    using beast::hash_append;
    hash_append(h, o.issue, o.strike, o.expiration);
}

/** Equality comparison. */
/** @{ */
[[nodiscard]] inline constexpr bool
operator==(Option const& lhs, Option const& rhs)
{
    return (lhs.issue == rhs.issue) && (lhs.strike == rhs.strike) &&
        (lhs.expiration == rhs.expiration);
}
/** @} */

/** Strict weak ordering. */
/** @{ */
[[nodiscard]] inline constexpr std::weak_ordering
operator<=>(Option const& lhs, Option const& rhs)
{
    if (auto const c{lhs.strike <=> rhs.strike}; c != 0)
        return c;
    return lhs.strike <=> rhs.strike;
}
/** @} */

}  // namespace xrpl

//------------------------------------------------------------------------------

namespace boost {

template <>
struct hash<xrpl::Option> : std::hash<xrpl::Option>
{
    using Base = std::hash<xrpl::Option>;
};

}  // namespace boost
