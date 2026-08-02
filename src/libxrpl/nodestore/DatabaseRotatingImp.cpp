#include <xrpl/nodestore/detail/DatabaseRotatingImp.h>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/contract.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/config/BasicConfig.h>
#include <xrpl/nodestore/Backend.h>
#include <xrpl/nodestore/Database.h>
#include <xrpl/nodestore/DatabaseRotating.h>
#include <xrpl/nodestore/NodeObject.h>
#include <xrpl/nodestore/Scheduler.h>
#include <xrpl/nodestore/Types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xrpl::NodeStore {

namespace {

using Ring = std::vector<std::shared_ptr<Backend>>;

// Names of the ring, ordered oldest -> newest.
std::vector<std::string>
ringNames(Ring const& ring)
{
    std::vector<std::string> names;
    names.reserve(ring.size());
    for (auto const& backend : ring)
        names.push_back(backend->getName());
    return names;
}

}  // namespace

DatabaseRotatingImp::DatabaseRotatingImp(
    Scheduler& scheduler,
    int readThreads,
    std::vector<std::shared_ptr<Backend>> generations,
    Section const& config,
    beast::Journal j)
    : DatabaseRotating(scheduler, readThreads, config, j)
    , ring_(std::make_shared<Ring>(std::move(generations)))
{
    for (auto const& backend : *ring_)
    {
        if (backend)
            fdRequired_ += backend->fdRequired();
    }
}

void
DatabaseRotatingImp::advance(
    std::unique_ptr<NodeStore::Backend>&& newWritable,
    RingPersist const& persist)
{
    std::vector<std::string> names;
    {
        std::scoped_lock const lock(mutex_);
        // Copy-on-write: the prior writable stays in the ring as a sealed, read-only
        // generation; the new backend becomes the writable one at the back.
        auto next = std::make_shared<Ring>(*ring_);
        next->push_back(std::shared_ptr<Backend>(std::move(newWritable)));
        ring_ = std::move(next);
        names = ringNames(*ring_);
    }
    persist(names);
}

std::size_t
DatabaseRotatingImp::generationCount() const
{
    std::scoped_lock const lock(mutex_);
    return ring_->size();
}

void
DatabaseRotatingImp::beginRetire()
{
    std::scoped_lock const lock(mutex_);
    retiring_ = ring_->empty() ? nullptr : ring_->front();
    copyForwardCount_.store(0, std::memory_order_relaxed);
}

void
DatabaseRotatingImp::endRetire()
{
    std::scoped_lock const lock(mutex_);
    retiring_.reset();
}

void
DatabaseRotatingImp::retireOldest(RingPersist const& persist)
{
    // Keep the dropped generation alive until after persist so its directory is deleted
    // only once the shortened ring is durably recorded (a crash never leaves a persisted
    // name without its backend, nor a deleted backend still named in the ring).
    std::shared_ptr<Backend> dropped;
    std::vector<std::string> names;
    std::uint64_t copyForwards = 0;
    {
        std::scoped_lock const lock(mutex_);
        // Never drop the writable generation.
        if (ring_->size() <= 1)
            return;
        dropped = ring_->front();
        // Copy-on-write: publish a new ring with the oldest generation removed.
        auto next = std::make_shared<Ring>(ring_->begin() + 1, ring_->end());
        ring_ = std::move(next);
        dropped->setDeletePath();
        names = ringNames(*ring_);
        copyForwards = copyForwardCount_.exchange(0, std::memory_order_relaxed);
        // Release the retiring reference to the dropped backend so `dropped` holds the
        // last one and its directory is removed when this function returns.
        if (retiring_ == dropped)
            retiring_.reset();
    }

    if (copyForwards > 0)
    {
        JLOG(j_.warn()) << "online_delete: evacuated " << copyForwards
                        << " live nodes from the retired generation into the writable backend";
    }

    persist(names);
    // `dropped` is destroyed here: its directory (armed by setDeletePath) is removed.
}

std::string
DatabaseRotatingImp::getName() const
{
    std::scoped_lock const lock(mutex_);
    return ring_->back()->getName();
}

std::int32_t
DatabaseRotatingImp::getWriteLoad() const
{
    std::scoped_lock const lock(mutex_);
    return ring_->back()->getWriteLoad();
}

void
DatabaseRotatingImp::importDatabase(Database& source)
{
    auto const backend = [&] {
        std::scoped_lock const lock(mutex_);
        return ring_->back();
    }();

    importInternal(*backend, source);
}

void
DatabaseRotatingImp::sync()
{
    std::scoped_lock const lock(mutex_);
    ring_->back()->sync();
}

void
DatabaseRotatingImp::store(NodeObjectType type, Blob&& data, uint256 const& hash, std::uint32_t)
{
    auto nObj = NodeObject::createObject(type, std::move(data), hash);

    auto const backend = [&] {
        std::scoped_lock const lock(mutex_);
        return ring_->back();
    }();

    backend->store(nObj);
    storeStats(1, nObj->getData().size());
}

void
DatabaseRotatingImp::sweep()
{
    // Nothing to do.
}

std::shared_ptr<NodeObject>
DatabaseRotatingImp::fetchNodeObject(
    uint256 const& hash,
    std::uint32_t,
    FetchReport& fetchReport,
    bool)
{
    auto fetch = [&](std::shared_ptr<Backend> const& backend) {
        Status status = Status::Ok;
        std::shared_ptr<NodeObject> nodeObject;
        try
        {
            status = backend->fetch(hash, &nodeObject);
        }
        catch (std::exception const& e)
        {
            JLOG(j_.fatal()) << "Exception, " << e.what();
            rethrow();
        }

        switch (status)
        {
            case Status::Ok:
            case Status::NotFound:
                break;
            case Status::DataCorrupt:
                JLOG(j_.fatal()) << "Corrupt NodeObject #" << hash;
                break;
            default:
                JLOG(j_.warn()) << "Unknown status=" << static_cast<int>(status);
                break;
        }

        return nodeObject;
    };

    // Take a shared_ptr copy of the immutable ring snapshot (plus the retiring
    // generation and writable backend) under the lock, then probe lock-free with no
    // allocation.
    std::shared_ptr<Ring const> ring;
    std::shared_ptr<Backend> retiring;
    std::shared_ptr<Backend> writable;
    {
        std::scoped_lock const lock(mutex_);
        ring = ring_;
        retiring = retiring_;
        writable = ring_->empty() ? nullptr : ring_->back();
    }

    // Probe newest -> oldest; first hit wins.
    std::shared_ptr<NodeObject> nodeObject;
    for (auto it = ring->rbegin(); it != ring->rend(); ++it)
    {
        auto const& backend = *it;
        nodeObject = fetch(backend);
        if (!nodeObject)
            continue;

        // Copy forward only when the hit is served by the retiring generation: it is
        // about to be dropped, so its still-live nodes must be preserved in the writable
        // backend. Reads served by other sealed generations are deliberately NOT copied,
        // which is what keeps evacuation O(churn) rather than O(total state).
        if (retiring && backend == retiring && writable && backend != writable)
        {
            writable->store(nodeObject);
            copyForwardCount_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }

    if (nodeObject)
        fetchReport.wasFound = true;

    return nodeObject;
}

void
DatabaseRotatingImp::forEach(std::function<void(std::shared_ptr<NodeObject>)> f)
{
    std::shared_ptr<Ring const> ring;
    {
        std::scoped_lock const lock(mutex_);
        ring = ring_;
    }

    for (auto const& backend : *ring)
        backend->forEach(f);
}

}  // namespace xrpl::NodeStore
