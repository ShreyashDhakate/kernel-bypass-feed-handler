// memory_hierarchy --- establish the memory-access cost baseline for this host.
//
// Every latency budget in this project is denominated in cache misses, so the
// first thing worth measuring is what a miss actually costs here. Two probes:
//
//   1. traversal order --- identical arithmetic over identical data, walked in
//      two different orders. Isolates the cost of spatial locality alone.
//
//   2. hierarchy latency --- a dependent-load pointer chase over a randomly
//      permuted cycle of cache-line-sized nodes. Because each load's address
//      comes from the previous load's result, the hardware prefetcher cannot
//      run ahead and the memory-level parallelism is pinned at one. The result
//      is the unloaded latency of a single miss at each working-set size, and
//      the cache capacity boundaries show up as steps in that curve.
//
//   build/bin/memory_hierarchy          # human-readable report
//   build/bin/memory_hierarchy --csv    # machine-readable, for plotting

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

constexpr size_t kKiB = 1024;
constexpr size_t kMiB = 1024 * kKiB;
constexpr size_t kCacheLine = 64;

// Forces the compiler to treat `value` as observably used, so a measured loop
// whose result is otherwise discarded cannot be optimised away. The empty asm
// block emits no instructions; it only constrains the optimiser.
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// ---------------------------------------------------------------------------
// Probe 1: traversal order
// ---------------------------------------------------------------------------
// A matrix is a single contiguous run of bytes, row after row. Walking along a
// row advances by one element; walking down a column advances by one full row,
// which on this matrix is 16 KiB. Same element count, same additions, same
// total bytes -- only the order differs.
//
// Note the barrier at the end of each outer iteration. GCC performs loop
// interchange at -O3: given the column-major nest it will simply swap the two
// loops and execute the row-major order instead, which makes both timings
// identical and the measurement meaningless. A memory clobber between the
// loops blocks that rewrite while leaving the inner loop free to vectorise, so
// the access pattern that runs is the one written here.
void probe_traversal_order(bool csv) {
    constexpr size_t kDim = 4096;  // 4096 x 4096 int32 = 64 MiB
    std::vector<int32_t> matrix(kDim * kDim, 1);
    do_not_optimize(matrix.data());  // contents are not compile-time knowable

    auto start = Clock::now();
    int64_t row_sum = 0;
    for (size_t row = 0; row < kDim; ++row) {
        for (size_t col = 0; col < kDim; ++col) {
            row_sum += matrix[row * kDim + col];
        }
        do_not_optimize(row_sum);
    }
    const double row_major_s = elapsed_seconds(start);

    start = Clock::now();
    int64_t col_sum = 0;
    for (size_t col = 0; col < kDim; ++col) {
        for (size_t row = 0; row < kDim; ++row) {
            col_sum += matrix[row * kDim + col];
        }
        do_not_optimize(col_sum);
    }
    const double col_major_s = elapsed_seconds(start);

    const double elements = static_cast<double>(kDim) * static_cast<double>(kDim);

    if (csv) {
        std::printf("traversal,row_major,ns_per_element,%.3f\n", row_major_s * 1e9 / elements);
        std::printf("traversal,col_major,ns_per_element,%.3f\n", col_major_s * 1e9 / elements);
        return;
    }

    std::printf("probe 1: traversal order\n");
    std::printf("  matrix        %zu x %zu int32 (%zu MiB), %.0f additions each way\n",
                kDim, kDim, matrix.size() * sizeof(int32_t) / kMiB, elements);
    std::printf("  %-12s %9.4f s   %7.3f ns/element\n", "row-major", row_major_s,
                row_major_s * 1e9 / elements);
    std::printf("  %-12s %9.4f s   %7.3f ns/element\n", "column-major", col_major_s,
                col_major_s * 1e9 / elements);
    std::printf("  ratio        %9.1fx slower for identical work\n", col_major_s / row_major_s);
    std::printf("  checksum     %lld / %lld (must match)\n\n",
                static_cast<long long>(row_sum), static_cast<long long>(col_sum));
}

// ---------------------------------------------------------------------------
// Probe 2: hierarchy latency
// ---------------------------------------------------------------------------
// One node per cache line, so no two hops ever share a line.
struct Node {
    uint32_t next;
    char     padding[kCacheLine - sizeof(uint32_t)];
};

static_assert(sizeof(Node) == kCacheLine, "node must occupy exactly one cache line");

// Links every node into a single random Hamiltonian cycle and walks it.
// Returns nanoseconds per hop, taking the best of `repeats` runs to suppress
// scheduler noise (the minimum is the cleanest estimator of true latency).
double measure_chase_ns(size_t bytes, int repeats, std::mt19937& rng) {
    const size_t nodes = bytes / sizeof(Node);
    std::vector<Node> buffer(nodes);

    std::vector<uint32_t> order(nodes);
    std::iota(order.begin(), order.end(), 0u);
    std::shuffle(order.begin(), order.end(), rng);
    for (size_t i = 0; i < nodes; ++i) {
        buffer[order[i]].next = order[(i + 1) % nodes];
    }

    const int64_t hops = 8'000'000;
    double best_ns = 1e30;
    uint32_t cursor = 0;

    for (int r = 0; r < repeats; ++r) {
        for (size_t i = 0; i < nodes; ++i) cursor = buffer[cursor].next;  // warm caches/TLB

        const auto start = Clock::now();
        for (int64_t i = 0; i < hops; ++i) cursor = buffer[cursor].next;
        const double secs = elapsed_seconds(start);

        best_ns = std::min(best_ns, secs * 1e9 / static_cast<double>(hops));
    }

    do_not_optimize(cursor);  // keep the dependent chain alive
    return best_ns;
}

void probe_hierarchy_latency(bool csv) {
    const size_t sizes[] = {8 * kKiB,   16 * kKiB,  32 * kKiB, 64 * kKiB,  128 * kKiB,
                            256 * kKiB, 512 * kKiB, 1 * kMiB,  2 * kMiB,   4 * kMiB,
                            8 * kMiB,   16 * kMiB,  32 * kMiB, 64 * kMiB,  128 * kMiB};

    std::mt19937 rng(0x5EED'1234u);  // fixed seed: the permutation is reproducible

    if (!csv) {
        std::printf("probe 2: dependent-load latency by working-set size\n");
        std::printf("  %10s  %12s\n", "working set", "ns per hop");
    }

    for (size_t bytes : sizes) {
        const double ns = measure_chase_ns(bytes, 3, rng);
        if (csv) {
            std::printf("hierarchy,%zu,ns_per_hop,%.3f\n", bytes, ns);
        } else if (bytes >= kMiB) {
            std::printf("  %7zu MiB  %12.2f\n", bytes / kMiB, ns);
        } else {
            std::printf("  %7zu KiB  %12.2f\n", bytes / kKiB, ns);
        }
    }
    if (!csv) std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool csv = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--csv") csv = true;
    }

    if (csv) {
        std::printf("probe,case,metric,value\n");
    } else {
        std::printf("memory hierarchy benchmark\n\n");
    }
    probe_traversal_order(csv);
    probe_hierarchy_latency(csv);
    return 0;
}
