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

#ifndef RIPPLE_APP_RDB_BATCH_H_INCLUDED
#define RIPPLE_APP_RDB_BATCH_H_INCLUDED

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/DatabaseCon.h>

#include <xrpl/protocol/Indexes.h>

namespace ripple {

std::unique_ptr<DatabaseCon>
makeBatchDB(DatabaseCon::Setup const& setup, beast::Journal j);

void
addBatchTxn(
    soci::session& session,
    TxID const& parentBatchId,
    TxID const& innerTxn,
    TER const& ter);

std::map<TxID, TER>
getBatchByParentID(
    soci::session& session,
    TxID const& parentBatchId);

}  // namespace ripple

#endif
