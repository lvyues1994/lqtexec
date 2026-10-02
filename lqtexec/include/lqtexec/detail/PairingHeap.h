#pragma once

#include <utility>

namespace lqtexec::detail {

// The links of a node in an intrusive pairing heap.
struct PairingHeapHook {
    PairingHeapHook *child = nullptr;
    PairingHeapHook *next = nullptr;
    // The previous sibling, or the parent of a first child; null for the root.
    PairingHeapHook *prev = nullptr;
};

// An intrusive min-heap: pushing, popping and erasing any node never allocate.
// Node derives from PairingHeapHook; Less is a stateless strict weak order on Nodes.
template <class Node, class Less>
struct PairingHeap {
    bool empty() const noexcept { return root == nullptr; }

    Node *top() const noexcept { return static_cast<Node *>(root); }

    void push(Node *const node) noexcept {
        PairingHeapHook *const hook = node;
        hook->child = nullptr;
        hook->next = nullptr;
        hook->prev = nullptr;
        root = meld(root, hook);
    }

    Node *pop() noexcept {
        auto *const old = root;
        root = mergePairs(old->child);
        old->child = nullptr;
        return static_cast<Node *>(old);
    }

    void erase(Node *const node) noexcept {
        PairingHeapHook *const hook = node;
        if (hook == root) {
            pop();
            return;
        }
        if (hook->prev->child == hook) {
            hook->prev->child = hook->next;
        } else {
            hook->prev->next = hook->next;
        }
        if (hook->next != nullptr) {
            hook->next->prev = hook->prev;
        }
        auto *const orphans = mergePairs(hook->child);
        hook->child = nullptr;
        hook->next = nullptr;
        hook->prev = nullptr;
        root = meld(root, orphans);
    }

private:
    static bool less(PairingHeapHook const *const a, PairingHeapHook const *const b) noexcept {
        return Less{}(*static_cast<Node const *>(a), *static_cast<Node const *>(b));
    }

    // Both arguments are roots: their next and prev are null.
    static PairingHeapHook *meld(PairingHeapHook *a, PairingHeapHook *b) noexcept {
        if (a == nullptr) {
            return b;
        }
        if (b == nullptr) {
            return a;
        }
        if (less(b, a)) {
            std::swap(a, b);
        }
        b->prev = a;
        b->next = a->child;
        if (a->child != nullptr) {
            a->child->prev = b;
        }
        a->child = b;
        return a;
    }

    // Two passes: meld the siblings in pairs from the left, then the pairs from the right.
    static PairingHeapHook *mergePairs(PairingHeapHook *first) noexcept {
        PairingHeapHook *pairs = nullptr;
        while (first != nullptr) {
            auto *const a = first;
            auto *const b = a->next;
            first = b != nullptr ? b->next : nullptr;
            a->next = nullptr;
            a->prev = nullptr;
            if (b != nullptr) {
                b->next = nullptr;
                b->prev = nullptr;
            }
            auto *const pair = meld(a, b);
            pair->next = pairs;
            pairs = pair;
        }
        PairingHeapHook *result = nullptr;
        while (pairs != nullptr) {
            auto *const pair = pairs;
            pairs = pair->next;
            pair->next = nullptr;
            result = meld(result, pair);
        }
        return result;
    }

    PairingHeapHook *root = nullptr;
};

} // namespace lqtexec::detail
