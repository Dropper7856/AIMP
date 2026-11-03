#pragma once
#include <cstddef>

namespace aimp::replication_core {
// A transaction is indivisible and may cross the ROOT budget by its size.
// Zero means the FIFO head is blocked/empty; never poll past it.
template<class Pump, class Clock>
std::size_t DrainOrderedTransactionBatch(std::size_t budget, Pump pump, Clock clock) noexcept {
    const auto started = clock();
    std::size_t processed = 0U;
    while (processed < budget) {
        const auto count = pump(budget - processed);
        if (count == 0U) break;
        processed += count;
        if (clock() - started >= 2U) break;
    }
    return processed;
}
}
