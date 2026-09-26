#include "shared_cockpit/dataref_learner.h"

#include <algorithm>
#include <cmath>

namespace flytogether {

void DatarefLearner::Begin(size_t candidate_count, double now_s) {
    candidates_.assign(candidate_count, Candidate{});
    start_s_ = now_s;
    next_order_ = 1;
    active_ = true;
}

bool DatarefLearner::Differs(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) {
        return true;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        // Relative tolerance: float datarefs jitter in the last bits even
        // when nothing really changed.
        const double scale = std::max(1.0, std::max(std::fabs(a[i]), std::fabs(b[i])));
        if (std::fabs(a[i] - b[i]) > 1e-4 * scale) {
            return true;
        }
    }
    return false;
}

void DatarefLearner::Observe(size_t index, const std::vector<double>& values, double now_s) {
    if (!active_ || index >= candidates_.size()) {
        return;
    }
    Candidate& c = candidates_[index];
    if (c.noisy) {
        return;
    }
    if (!c.has_value) {
        c.has_value = true;
        c.baseline = values;
        c.last = values;
        return;
    }
    if (!Differs(values, c.last)) {
        return;
    }
    if (now_s - start_s_ < kBaselineS) {
        c.noisy = true; // moved while nobody was touching anything
        return;
    }
    c.last = values;
    if (++c.change_count > kMaxChanges) {
        c.noisy = true; // keeps moving on its own - a gauge, not a switch
        return;
    }
    if (c.first_change_order == 0) {
        c.first_change_order = next_order_++;
    }
}

std::vector<DatarefLearner::Change> DatarefLearner::Changes() const {
    std::vector<std::pair<size_t, size_t>> order; // (first_change_order, index)
    for (size_t i = 0; i < candidates_.size(); ++i) {
        const Candidate& c = candidates_[i];
        if (!c.noisy && c.first_change_order != 0) {
            order.emplace_back(c.first_change_order, i);
        }
    }
    std::sort(order.begin(), order.end());
    std::vector<Change> out;
    out.reserve(order.size());
    for (const auto& [unused, i] : order) {
        const Candidate& c = candidates_[i];
        out.push_back(Change{i, c.baseline, c.last, c.change_count});
    }
    return out;
}

size_t DatarefLearner::noisy_count() const {
    return static_cast<size_t>(
        std::count_if(candidates_.begin(), candidates_.end(), [](const Candidate& c) { return c.noisy; }));
}

} // namespace flytogether
