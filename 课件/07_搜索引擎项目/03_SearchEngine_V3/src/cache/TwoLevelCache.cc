#include "../../include/cache/TwoLevelCache.h"

TwoLevelCache::TwoLevelCache(Cache* l1, Cache* l2, int l1BackfillTtlSeconds)
    : l1_(l1)
    , l2_(l2)
    , l1BackfillTtlSeconds_(l1BackfillTtlSeconds)
{
}

bool TwoLevelCache::get(const std::string& key, std::string& value)
{
    if (l1_ != nullptr && l1_->get(key, value)) {
        return true;
    }

    if (l2_ != nullptr && l2_->get(key, value)) {
        if (l1_ != nullptr) {
            l1_->put(key, value, l1BackfillTtlSeconds_);
        }
        return true;
    }

    return false;
}

void TwoLevelCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    if (l1_ != nullptr) {
        l1_->put(key, value, ttlSeconds);
    }
    if (l2_ != nullptr) {
        l2_->put(key, value, ttlSeconds);
    }
}

void TwoLevelCache::erase(const std::string& key)
{
    if (l1_ != nullptr) {
        l1_->erase(key);
    }
    if (l2_ != nullptr) {
        l2_->erase(key);
    }
}
