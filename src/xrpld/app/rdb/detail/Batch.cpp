//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2021 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/rdb/Batch.h>

#include <boost/format.hpp>

namespace ripple {

std::unique_ptr<DatabaseCon>
makeBatchDB(DatabaseCon::Setup const& setup, beast::Journal j)
{
    // batch database
    return std::make_unique<DatabaseCon>(
        setup, BatchDBName, std::array<std::string, 0>(), BatchDBInit, j);
}

void
addBatchTxn(
    soci::session& session,
    TxID const& parentBatchId,
    TxID const& innerTxn,
    TER const& ter)
{
    soci::transaction tr(session);

    // Convert TxID and TER to appropriate formats
    std::string const parentHex = to_string(parentBatchId);
    std::string const innerHex = to_string(innerTxn);
    int const terValue = TERtoInt(ter);

    session << "INSERT OR REPLACE INTO BatchTransactions "
               "(ParentBatchID, InnerTxnID, TERResult) VALUES "
               "(:parentId, :txnId, :result)",
        soci::use(parentHex), soci::use(innerHex), soci::use(terValue);

    tr.commit();
}

std::map<TxID, TER>
getBatchByParentID(soci::session& session, TxID const& parentBatchId)
{
    std::map<TxID, TER> ret;

    std::string const parentHex = to_string(parentBatchId);

    std::string innerTxnHex;
    int terResult;

    soci::statement st =
        (session.prepare
             << "SELECT InnerTxnID, TERResult FROM BatchTransactions "
                "WHERE ParentBatchID = :parentId",
         soci::use(parentHex),
         soci::into(innerTxnHex),
         soci::into(terResult));

    st.execute();

    while (st.fetch())
    {
        TxID innerTxnId;
        if (innerTxnId.parseHex(innerTxnHex))
        {
            ret.emplace(innerTxnId, TER::fromInt(terResult));
        }
    }

    return ret;
}

}  // namespace ripple
