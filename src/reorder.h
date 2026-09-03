#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace lightlaunch {

inline std::size_t RemapIndexAfterMove(std::size_t index,
                                       std::size_t source,
                                       std::size_t destination) {
    if (index == source) {
        return destination;
    }
    if (source < destination && index > source && index <= destination) {
        return index - 1;
    }
    if (source > destination && index >= destination && index < source) {
        return index + 1;
    }
    return index;
}

template <typename T>
bool MoveVectorElement(std::vector<T>& values, std::size_t source,
                       std::size_t destination) {
    if (source >= values.size() || destination >= values.size() ||
        source == destination) {
        return false;
    }
    T moved = std::move(values[source]);
    values.erase(values.begin() + static_cast<std::ptrdiff_t>(source));
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(destination),
                  std::move(moved));
    return true;
}

}  // namespace lightlaunch
