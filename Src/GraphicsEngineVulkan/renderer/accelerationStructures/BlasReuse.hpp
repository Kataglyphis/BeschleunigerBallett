#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace Kataglyphis {

// Owner identity, not address: a reload frees the old model, and its successor may reuse the address.
template<typename T>
[[nodiscard]] auto reusableBlasPrefix(const std::vector<std::weak_ptr<T>> &builtFrom,
  const std::vector<std::shared_ptr<T>> &models) -> std::size_t
{
    const std::size_t limit = std::min(builtFrom.size(), models.size());
    std::size_t kept = 0;
    while (kept < limit && models[kept] != nullptr && !builtFrom[kept].owner_before(models[kept])
           && !models[kept].owner_before(builtFrom[kept])) {
        ++kept;
    }
    return kept;
}

}// namespace Kataglyphis
