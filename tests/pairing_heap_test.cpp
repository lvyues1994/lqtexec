#include <lqtexec/detail/PairingHeap.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace {

struct Node : lqtexec::detail::PairingHeapHook {
    int key = 0;
    int id = 0;
    bool inHeap = false;
};

struct NodeLess {
    bool operator()(Node const &a, Node const &b) const noexcept {
        return a.key < b.key or (a.key == b.key and a.id < b.id);
    }
};

using Heap = lqtexec::detail::PairingHeap<Node, NodeLess>;

} // namespace

TEST_CASE("pairing heap pops in order") {
    auto nodes = std::vector<Node>(100);
    auto heap = Heap{};
    for (auto i = 0; i < 100; ++i) {
        nodes[static_cast<std::size_t>(i)].key = (i * 37) % 100;
        nodes[static_cast<std::size_t>(i)].id = i;
        heap.push(&nodes[static_cast<std::size_t>(i)]);
    }
    for (auto expected = 0; expected < 100; ++expected) {
        REQUIRE_FALSE(heap.empty());
        CHECK(heap.pop()->key == expected);
    }
    CHECK(heap.empty());
}

TEST_CASE("pairing heap matches an ordered set under random pushes, pops and erasures") {
    auto rng = std::mt19937{12345};
    auto nodes = std::vector<Node>(512);
    for (auto i = 0; i < 512; ++i) {
        nodes[static_cast<std::size_t>(i)].id = i;
    }
    auto heap = Heap{};
    auto oracle = std::set<std::pair<int, int>>{};
    auto pick = std::uniform_int_distribution<std::size_t>{0, nodes.size() - 1};
    auto keys = std::uniform_int_distribution<int>{0, 50};
    auto action = std::uniform_int_distribution<int>{0, 2};

    for (auto step = 0; step < 20000; ++step) {
        auto &node = nodes[pick(rng)];
        switch (action(rng)) {
        case 0:
            if (not node.inHeap) {
                node.key = keys(rng);
                node.inHeap = true;
                heap.push(&node);
                oracle.emplace(node.key, node.id);
            }
            break;
        case 1:
            if (node.inHeap) {
                heap.erase(&node);
                node.inHeap = false;
                oracle.erase({node.key, node.id});
            }
            break;
        default:
            if (not oracle.empty()) {
                auto *const top = heap.pop();
                top->inHeap = false;
                REQUIRE(std::pair{top->key, top->id} == *oracle.begin());
                oracle.erase(oracle.begin());
            }
            break;
        }
        REQUIRE(heap.empty() == oracle.empty());
        if (not oracle.empty()) {
            REQUIRE(std::pair{heap.top()->key, heap.top()->id} == *oracle.begin());
        }
    }
}
