#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>

#include <cstddef>
#include <cstdint>
#include <set>

namespace xrpl {

class Application;
class ReadView;

/** Check if a validator is trusted for export signing.

    Three-tier trust model:
    1. Standalone mode: always trusted
    2. Early ledgers (seq < 256): use local configured UNL
    3. Normal operation: use on-chain UNLReport, fallback to local UNL

    @param view The current ledger view
    @param app  The application instance
    @param validator The validator's public key (signing or master)
    @param j    Journal for logging
    @return true if the validator is trusted for export signing
*/
bool
isExportValidatorTrusted(
    ReadView const& view,
    Application& app,
    PublicKey const& validator,
    beast::Journal const& j);

/** Get the size of the export validator set.

    Returns the number of validators trusted for export signing.
    Uses UNLReport if available, otherwise falls back to local UNL.

    @param view The current ledger view
    @param app  The application instance
    @return The number of trusted export validators (minimum 1)
*/
std::size_t
getExportUNLSize(ReadView const& view, Application& app);

/** Get the set of validator keys trusted for export signing.

    Returns public keys from UNLReport if available,
    otherwise from local trusted validator set.

    @param view The current ledger view
    @param app  The application instance
    @return Set of validator public keys
*/
std::set<PublicKey>
getExportValidatorSet(ReadView const& view, Application& app);

/** Calculate the export quorum for a given validator count.

    Uses integer arithmetic: ceil(count * 80%).
    Formula: (count * 80 + 99) / 100

    @param count The number of validators
    @return The quorum threshold
*/
inline std::uint32_t
calculateExportQuorum(std::size_t count)
{
    return static_cast<std::uint32_t>((count * 80 + 99) / 100);
}

}  // namespace xrpl
