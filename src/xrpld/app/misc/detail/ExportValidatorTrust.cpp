#include <xrpld/app/misc/ExportValidatorTrust.h>

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ValidatorList.h>

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STArray.h>

namespace xrpl {

bool
isExportValidatorTrusted(
    ReadView const& view,
    Application& app,
    PublicKey const& validator,
    beast::Journal const& j)
{
    // Standalone mode: always trusted
    if (app.config().standalone())
        return true;

    // Resolve master key from signing key using manifests
    auto const master = app.validatorManifests().getMasterKey(validator);

    auto const trustedLocal = app.validators().trusted(validator) ||
        (master != validator && app.validators().trusted(master));

    // Early-ledger fallback: trust local configured UNL
    if (view.seq() < 256)
        return trustedLocal;

    // Read UNLReport from ledger
    auto const unlReport = view.read(keylet::UNLReport());
    if (!unlReport || !unlReport->isFieldPresent(sfActiveValidators))
    {
        JLOG(j.debug())
            << "Export: UNLReport missing; using local trust set";
        return trustedLocal;
    }

    // Check if the validator (signing key or master key) is in UNLReport
    auto const signerId = calcAccountID(validator);
    auto const masterId = calcAccountID(master);
    auto const& active = unlReport->getFieldArray(sfActiveValidators);
    for (auto const& av : active)
    {
        auto const id = av.getAccountID(sfAccount);
        if (id == signerId || id == masterId)
            return true;
    }

    return false;
}

std::size_t
getExportUNLSize(ReadView const& view, Application& app)
{
    if (app.config().standalone())
        return 1;

    auto const unlReport = view.read(keylet::UNLReport());
    if (unlReport && unlReport->isFieldPresent(sfActiveValidators))
        return unlReport->getFieldArray(sfActiveValidators).size();

    auto const localTrusted =
        app.validators().getTrustedMasterKeys().size();
    return localTrusted > 0 ? localTrusted : 1;
}

std::set<PublicKey>
getExportValidatorSet(ReadView const& view, Application& app)
{
    std::set<PublicKey> result;

    if (app.config().standalone())
        return result;

    auto const unlReport = view.read(keylet::UNLReport());
    if (unlReport && unlReport->isFieldPresent(sfActiveValidators))
    {
        auto const& active =
            unlReport->getFieldArray(sfActiveValidators);
        for (auto const& av : active)
        {
            auto const pkBlob = av.getFieldVL(sfPublicKey);
            if (publicKeyType(makeSlice(pkBlob)))
                result.emplace(makeSlice(pkBlob));
        }
        return result;
    }

    // Fallback to local trusted set
    auto const& keys = app.validators().getTrustedMasterKeys();
    return std::set<PublicKey>(keys.begin(), keys.end());
}

}  // namespace xrpl
