#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#include "order_book.hpp"

using namespace matching_engine;

namespace {

using Clock = std::chrono::steady_clock;
using Nanos = std::int64_t;

// Reproducible stream of random limit orders. Prices are drawn directly in
// ticks, so the benchmark exercises the same integer grid the book uses.
class OrderStream {
   public:
    explicit OrderStream(std::uint32_t seed) : rng_(seed) {}

    Side side() { return side_dist_(rng_) == 0 ? Side::Buy : Side::Sell; }
    Price price() { return price_dist_(rng_); }
    Quantity quantity() { return static_cast<Quantity>(qty_dist_(rng_)); }

   private:
    std::mt19937 rng_;
    std::uniform_int_distribution<Price> price_dist_{95'00, 105'00};
    std::uniform_int_distribution<int> qty_dist_{1, 100};
    std::uniform_int_distribution<int> side_dist_{0, 1};
};

// Nearest-rank percentile over an already-sorted sample: the smallest
// observation at or above the q-th fraction. No interpolation -- these are
// latencies, and reporting a value that was never actually measured would be
// worse than a slightly coarse one.
Nanos percentile(const std::vector<Nanos>& sorted, double q) {
    if (sorted.empty()) return 0;
    std::size_t rank = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
    if (rank >= sorted.size()) rank = sorted.size() - 1;
    return sorted[rank];
}

// Cost of the measurement itself: two clock reads with no work between them.
// Reported alongside the percentiles because at these magnitudes it is not a
// rounding error -- it is a meaningful share of what is being measured.
Nanos clockOverhead() {
    constexpr int kSamples = 10000;
    std::vector<Nanos> samples;
    samples.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const auto start = Clock::now();
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return percentile(samples, 0.5);
}

}  // namespace

int main(int argc, char** argv) {
    const int num_orders = (argc > 1) ? std::stoi(argv[1]) : 1'000'000;
    if (num_orders <= 0) {
        std::cerr << "order count must be positive\n";
        return 1;
    }

    OrderId next_id = 0;

    // ---- Throughput: one timer around the whole loop, so per-order clock
    // reads don't inflate the result.
    {
        OrderBook book;
        OrderStream stream(42);  // fixed seed: reproducible numbers to quote
        const auto start = Clock::now();
        for (int i = 0; i < num_orders; ++i) {
            book.addLimitOrder(next_id++, stream.side(), stream.price(), stream.quantity());
        }
        const auto end = Clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();

        std::cout << "Throughput\n";
        std::cout << "  " << num_orders << " orders in " << seconds << "s\n";
        std::cout << "  " << static_cast<long long>(num_orders / seconds) << " orders/sec\n";
        std::cout << "  resting orders remaining: " << book.restingOrderCount() << "\n\n";
    }

    // ---- Latency: every order timed individually, against a book that has
    // already been warmed to a realistic depth rather than an empty one.
    {
        OrderBook book;
        OrderStream stream(42);
        const int warmup = num_orders / 2;
        for (int i = 0; i < warmup; ++i) {
            book.addLimitOrder(next_id++, stream.side(), stream.price(), stream.quantity());
        }

        std::vector<Nanos> samples;
        samples.reserve(static_cast<std::size_t>(num_orders));
        for (int i = 0; i < num_orders; ++i) {
            const Side side = stream.side();
            const Price price = stream.price();
            const Quantity qty = stream.quantity();

            const auto start = Clock::now();
            book.addLimitOrder(next_id++, side, price, qty);
            const auto end = Clock::now();
            samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }
        std::sort(samples.begin(), samples.end());

        std::cout << "Latency per addLimitOrder (book warmed with " << warmup << " orders)\n";
        std::cout << "  p50    " << percentile(samples, 0.50) << " ns\n";
        std::cout << "  p90    " << percentile(samples, 0.90) << " ns\n";
        std::cout << "  p99    " << percentile(samples, 0.99) << " ns\n";
        std::cout << "  p99.9  " << percentile(samples, 0.999) << " ns\n";
        std::cout << "  max    " << samples.back() << " ns\n";
        std::cout << "  (clock overhead, subtract from the above: " << clockOverhead() << " ns)\n";
    }

    return 0;
}
