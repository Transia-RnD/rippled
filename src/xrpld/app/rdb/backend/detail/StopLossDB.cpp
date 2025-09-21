#include <xrpld/app/rdb/backend/StopLossDB.h>

#include <xrpl/protocol/Serializer.h>

#include <lmdb.h>

#include <cstring>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

namespace ripple {

class StopLossDB::Impl
{
public:
    Impl(const std::string& dbPath, unsigned int maxDBs, size_t mapSize)
        : dbPath_(dbPath), maxDBs_(maxDBs), mapSize_(mapSize), env_(nullptr)
    {
    }

    ~Impl()
    {
        close();
    }

    bool
    init()
    {
        int rc = mdb_env_create(&env_);
        if (rc != 0)
        {
            std::cerr << "Failed to create LMDB environment: " << rc
                      << std::endl;
            return false;
        }

        rc = mdb_env_set_maxdbs(env_, maxDBs_);
        if (rc != 0)
        {
            std::cerr << "Failed to set max DBs: " << rc << std::endl;
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        rc = mdb_env_set_mapsize(env_, mapSize_);
        if (rc != 0)
        {
            std::cerr << "Failed to set map size: " << rc << std::endl;
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        rc = mdb_env_open(env_, dbPath_.c_str(), 0, 0664);
        if (rc != 0)
        {
            std::cerr << "Failed to open LMDB environment: " << rc << std::endl;
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        MDB_txn* txn = nullptr;
        rc = mdb_txn_begin(env_, nullptr, 0, &txn);
        if (rc != 0)
        {
            std::cerr << "Failed to begin transaction: " << rc << std::endl;
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        // Create databases for each stop type - keep original structure
        rc = mdb_dbi_open(txn, "orders", MDB_CREATE, &ordersDb_);
        rc |= mdb_dbi_open(txn, "sell_stop", MDB_CREATE, &sellStopDb_);
        rc |= mdb_dbi_open(txn, "buy_stop", MDB_CREATE, &buyStopDb_);
        rc |= mdb_dbi_open(txn, "sell_tp", MDB_CREATE, &sellTPDb_);
        rc |= mdb_dbi_open(txn, "buy_tp", MDB_CREATE, &buyTPDb_);

        if (rc != 0)
        {
            std::cerr << "Failed to open databases: " << rc << std::endl;
            mdb_txn_abort(txn);
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        rc = mdb_txn_commit(txn);
        return rc == 0;
    }

    void
    close()
    {
        if (env_)
        {
            mdb_dbi_close(env_, ordersDb_);
            mdb_dbi_close(env_, sellStopDb_);
            mdb_dbi_close(env_, buyStopDb_);
            mdb_dbi_close(env_, sellTPDb_);
            mdb_dbi_close(env_, buyTPDb_);
            mdb_env_close(env_);
            env_ = nullptr;
        }
    }

    bool
    addOrder(
        uint256 const& txHash,
        std::vector<uint8_t> const& signedBlob,
        double stopPrice,
        StopType stopType,
        STAmount const& takerGets,
        STAmount const& takerPays)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!env_)
            return false;

        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0)
            return false;

        // Store full order data with txHash as key in orders db
        Serializer orderData;
        orderData.add32(static_cast<uint32_t>(stopType));
        uint64_t priceBits = *reinterpret_cast<uint64_t*>(&stopPrice);
        orderData.add64(priceBits);
        orderData.addRaw(
            takerGets.getCurrency().data(), takerGets.getCurrency().size());
        orderData.addRaw(
            takerGets.getIssuer().data(), takerGets.getIssuer().size());
        orderData.addRaw(
            takerPays.getCurrency().data(), takerPays.getCurrency().size());
        orderData.addRaw(
            takerPays.getIssuer().data(), takerPays.getIssuer().size());
        orderData.add32(static_cast<uint32_t>(signedBlob.size()));
        orderData.addRaw(signedBlob.data(), signedBlob.size());

        auto orderBytes = orderData.peekData();

        MDB_val key, data;
        key.mv_data = const_cast<uint8_t*>(txHash.data());
        key.mv_size = txHash.size();
        data.mv_data = const_cast<uint8_t*>(orderBytes.data());
        data.mv_size = orderBytes.size();

        if (mdb_put(txn, ordersDb_, &key, &data, MDB_NOOVERWRITE) != 0)
        {
            mdb_txn_abort(txn);
            return false;
        }

        // Create price index key for the appropriate database
        std::string indexKey = makePriceIndexKey(
            takerGets.getCurrency(),
            takerGets.getIssuer(),
            takerPays.getCurrency(),
            takerPays.getIssuer(),
            stopPrice,
            txHash);

        MDB_val indexKeyVal, indexDataVal;
        indexKeyVal.mv_data = const_cast<char*>(indexKey.data());
        indexKeyVal.mv_size = indexKey.size();
        indexDataVal.mv_data = const_cast<uint8_t*>(txHash.data());
        indexDataVal.mv_size = txHash.size();

        // Add to appropriate price index
        MDB_dbi targetIndex = getIndexDb(stopType);
        if (mdb_put(txn, targetIndex, &indexKeyVal, &indexDataVal, 0) != 0)
        {
            mdb_txn_abort(txn);
            return false;
        }

        return mdb_txn_commit(txn) == 0;
    }

    std::vector<StopLossOrder>
    getTriggeredOrders(
        double currentPrice,
        StopType stopType,
        Book const& market,
        size_t maxOrders)
    {
        std::vector<StopLossOrder> triggered;
        triggered.reserve(std::min(maxOrders, size_t(100)));

        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0)
            return triggered;

        MDB_dbi targetIndex = getIndexDb(stopType);
        MDB_cursor* cursor;

        if (mdb_cursor_open(txn, targetIndex, &cursor) != 0)
        {
            mdb_txn_abort(txn);
            return triggered;
        }

        // Create market prefix to filter to this specific market
        std::string marketPrefix = makeMarketPrefix(
            market.in.currency,
            market.in.account,
            market.out.currency,
            market.out.account);

        MDB_val key, data;

        // Position cursor at start of this market's orders
        key.mv_data = const_cast<char*>(marketPrefix.data());
        key.mv_size = marketPrefix.size();

        int rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);

        while (rc == 0 && triggered.size() < maxOrders)
        {
            std::string keyStr(static_cast<char*>(key.mv_data), key.mv_size);

            // Check if still in same market
            if (keyStr.substr(0, marketPrefix.size()) != marketPrefix)
                break;

            // Extract price from key
            size_t pricePos = marketPrefix.size();
            if (keyStr.size() < pricePos + 16)
            {
                rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                continue;
            }

            double orderPrice = std::stod(keyStr.substr(pricePos, 16));

            // Check if triggered based on stop type
            bool shouldTrigger = false;
            switch (stopType)
            {
                case StopType::STOP_LOSS_SELL:
                    shouldTrigger = currentPrice <= orderPrice;
                    if (!shouldTrigger)
                        goto done;  // Sorted ascending, can stop
                    break;
                case StopType::STOP_LOSS_BUY:
                    shouldTrigger = currentPrice >= orderPrice;
                    break;
                case StopType::TAKE_PROFIT_SELL:
                    shouldTrigger = currentPrice >= orderPrice;
                    break;
                case StopType::TAKE_PROFIT_BUY:
                    shouldTrigger = currentPrice <= orderPrice;
                    if (!shouldTrigger)
                        goto done;  // Sorted ascending, can stop
                    break;
            }

            if (shouldTrigger)
            {
                // Get the txHash from index value
                uint256 txHash;
                memcpy(txHash.data(), data.mv_data, data.mv_size);

                // Fetch full order data
                MDB_val orderKey, orderData;
                orderKey.mv_data = txHash.data();
                orderKey.mv_size = txHash.size();

                if (mdb_get(txn, ordersDb_, &orderKey, &orderData) == 0)
                {
                    StopLossOrder order;
                    if (parseOrder(orderData, order))
                    {
                        order.txHash = txHash;
                        triggered.push_back(std::move(order));
                    }
                }
            }

            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }

    done:
        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return triggered;
    }

    void
    removeOrders(std::vector<uint256> const& txHashes)
    {
        if (txHashes.empty())
            return;

        std::lock_guard<std::mutex> lock(mtx_);

        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0)
            return;

        for (auto const& txHash : txHashes)
        {
            // First get the order data to know which index to remove from
            MDB_val key, data;
            key.mv_data = const_cast<uint8_t*>(txHash.data());
            key.mv_size = txHash.size();

            if (mdb_get(txn, ordersDb_, &key, &data) == 0)
            {
                StopLossOrder order;
                if (parseOrder(data, order))
                {
                    // Remove from price index
                    MDB_dbi targetIndex = getIndexDb(order.stopType);
                    std::string indexKey = makePriceIndexKey(
                        order.takerGetsCurrency,
                        order.takerGetsIssuer,
                        order.takerPaysCurrency,
                        order.takerPaysIssuer,
                        order.stopPrice,
                        txHash);

                    MDB_val indexKeyVal;
                    indexKeyVal.mv_data = const_cast<char*>(indexKey.data());
                    indexKeyVal.mv_size = indexKey.size();

                    mdb_del(txn, targetIndex, &indexKeyVal, nullptr);
                }

                // Remove from main orders db
                mdb_del(txn, ordersDb_, &key, nullptr);
            }
        }

        mdb_txn_commit(txn);
    }

    size_t
    getOrderCount() const
    {
        if (!env_)
            return 0;

        MDB_txn* txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0)
            return 0;

        MDB_stat stat;
        if (mdb_stat(txn, ordersDb_, &stat) != 0)
        {
            mdb_txn_abort(txn);
            return 0;
        }

        size_t count = stat.ms_entries;
        mdb_txn_abort(txn);
        return count;
    }

    std::vector<Book>
    getMonitoredMarkets() const
    {
        std::vector<Book> markets;
        std::set<std::string> uniqueMarkets;

        if (!env_)
            return markets;

        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0)
            return markets;

        // Check all index databases
        MDB_dbi indices[] = {sellStopDb_, buyStopDb_, sellTPDb_, buyTPDb_};

        for (auto idx : indices)
        {
            MDB_cursor* cursor;
            if (mdb_cursor_open(txn, idx, &cursor) != 0)
                continue;

            MDB_val key, data;
            int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);

            while (rc == 0)
            {
                std::string keyStr(
                    static_cast<char*>(key.mv_data), key.mv_size);

                // Extract market identifier (first part before price)
                size_t priceStart =
                    keyStr.find_last_of('_', keyStr.size() - 65);
                if (priceStart != std::string::npos)
                {
                    std::string marketId = keyStr.substr(0, priceStart - 16);
                    if (uniqueMarkets.insert(marketId).second)
                    {
                        auto book = parseMarketFromKey(marketId);
                        if (book.in.currency != noCurrency() &&
                            book.out.currency != noCurrency())
                        {
                            markets.push_back(book);
                        }
                    }
                }

                rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
            }

            mdb_cursor_close(cursor);
        }

        mdb_txn_abort(txn);
        return markets;
    }

private:
    MDB_dbi
    getIndexDb(StopType type) const
    {
        switch (type)
        {
            case StopType::STOP_LOSS_SELL:
                return sellStopDb_;
            case StopType::STOP_LOSS_BUY:
                return buyStopDb_;
            case StopType::TAKE_PROFIT_SELL:
                return sellTPDb_;
            case StopType::TAKE_PROFIT_BUY:
                return buyTPDb_;
        }
        return sellStopDb_;
    }

    std::string
    makePriceIndexKey(
        Currency const& getCur,
        AccountID const& getIss,
        Currency const& payCur,
        AccountID const& payIss,
        double price,
        uint256 const& txHash)
    {
        std::ostringstream oss;
        oss << to_string(getCur) << "_" << to_string(getIss) << "_"
            << to_string(payCur) << "_" << to_string(payIss) << "_";
        oss << std::fixed << std::setprecision(10) << std::setw(16)
            << std::setfill('0') << price;
        oss << "_" << to_string(txHash);
        return oss.str();
    }

    std::string
    makeMarketPrefix(
        Currency const& getCur,
        AccountID const& getIss,
        Currency const& payCur,
        AccountID const& payIss)
    {
        std::ostringstream oss;
        oss << to_string(getCur) << "_" << to_string(getIss) << "_"
            << to_string(payCur) << "_" << to_string(payIss) << "_";
        return oss.str();
    }

    bool
    parseOrder(MDB_val const& data, StopLossOrder& order)
    {
        try
        {
            SerialIter sit(static_cast<uint8_t*>(data.mv_data), data.mv_size);

            order.stopType = static_cast<StopType>(sit.get32());
            uint64_t priceBits = sit.get64();
            order.stopPrice = *reinterpret_cast<double*>(&priceBits);

            auto getRawData = sit.getRaw(order.takerGetsCurrency.size());
            std::memcpy(
                order.takerGetsCurrency.data(),
                getRawData.data(),
                order.takerGetsCurrency.size());

            getRawData = sit.getRaw(order.takerGetsIssuer.size());
            std::memcpy(
                order.takerGetsIssuer.data(),
                getRawData.data(),
                order.takerGetsIssuer.size());

            getRawData = sit.getRaw(order.takerPaysCurrency.size());
            std::memcpy(
                order.takerPaysCurrency.data(),
                getRawData.data(),
                order.takerPaysCurrency.size());

            getRawData = sit.getRaw(order.takerPaysIssuer.size());
            std::memcpy(
                order.takerPaysIssuer.data(),
                getRawData.data(),
                order.takerPaysIssuer.size());

            uint32_t blobSize = sit.get32();
            order.signedTxBlob.resize(blobSize);
            auto blobData = sit.getRaw(blobSize);
            std::memcpy(order.signedTxBlob.data(), blobData.data(), blobSize);

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    Book
    parseMarketFromKey(std::string const& marketId) const
    {
        // Market ID format: getCur_getIss_payCur_payIss
        std::vector<std::string> parts;
        std::stringstream ss(marketId);
        std::string part;

        while (std::getline(ss, part, '_'))
            parts.push_back(part);

        if (parts.size() != 4)
            return Book{};

        try
        {
            Currency getCur;
            AccountID getIss;
            Currency payCur;
            AccountID payIss;

            // Parse currency and account strings
            if (!to_currency(getCur, parts[0]) ||
                !to_issuer(getIss, parts[1]) ||
                !to_currency(payCur, parts[2]) || !to_issuer(payIss, parts[3]))
            {
                return Book{};
            }

            return Book{Issue{getCur, getIss}, Issue{payCur, payIss}};
        }
        catch (...)
        {
            return Book{};
        }
    }

    std::string dbPath_;
    unsigned int maxDBs_;
    size_t mapSize_;
    MDB_env* env_;
    MDB_dbi ordersDb_;
    MDB_dbi sellStopDb_;
    MDB_dbi buyStopDb_;
    MDB_dbi sellTPDb_;
    MDB_dbi buyTPDb_;
    mutable std::mutex mtx_;
};

// Public interface implementation
StopLossDB::StopLossDB(
    const std::string& dbPath,
    unsigned int maxDBs,
    size_t mapSize)
    : pImpl(std::make_unique<Impl>(dbPath, maxDBs, mapSize))
{
}

StopLossDB::~StopLossDB() = default;

bool
StopLossDB::init()
{
    return pImpl->init();
}

void
StopLossDB::close()
{
    pImpl->close();
}

bool
StopLossDB::addOrder(
    uint256 const& txHash,
    std::vector<uint8_t> const& signedBlob,
    double stopPrice,
    StopType stopType,
    STAmount const& takerGets,
    STAmount const& takerPays)
{
    return pImpl->addOrder(
        txHash, signedBlob, stopPrice, stopType, takerGets, takerPays);
}

std::vector<StopLossOrder>
StopLossDB::getTriggeredOrders(
    double currentPrice,
    StopType stopType,
    Book const& market,
    size_t maxOrders)
{
    return pImpl->getTriggeredOrders(currentPrice, stopType, market, maxOrders);
}

void
StopLossDB::removeOrders(std::vector<uint256> const& txHashes)
{
    pImpl->removeOrders(txHashes);
}

size_t
StopLossDB::getOrderCount() const
{
    return pImpl->getOrderCount();
}

std::vector<Book>
StopLossDB::getMonitoredMarkets() const
{
    return pImpl->getMonitoredMarkets();
}

}  // namespace ripple