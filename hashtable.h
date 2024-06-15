#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>


// hashtable node, should be embedded into the payload
struct HNode {
    HNode* next = NULL;
    uint64_t hcode = 0;

    HNode(): next(nullptr), hcode(0) {}
    HNode(HNode* n, uint64_t hc): next(n), hcode(hc) {}
};

// a simple fixed-sized hashtable
struct HTab {
    HNode **tab;
    size_t mask;
    size_t size;

    HTab(): tab(nullptr), mask(0), size(0) {}

    HTab(size_t n) {
        // n must be a power of 2
        assert(n > 0 && ((n - 1) & n) == 0);
        tab = new HNode*[n];
        mask = n - 1;
        size = 0;
    }

    ~HTab() {
        delete[] tab;
        tab = nullptr;
    }

    void insert(HNode* node) {
        size_t pos = node->hcode & mask;
        HNode* first = tab[pos];
        node->next = first;
        tab[pos] = node;
        size++;
    }

    HNode** lookup(HNode* key, bool (*eq)(HNode*, HNode*)) {
        if (!tab) {
            return nullptr;
        }

        auto pos = key->hcode & mask;
        HNode** from = &tab[pos];
        HNode* curr{nullptr};
        for (; (curr = *from) != nullptr; from = &(curr->next)) {
            if (curr->hcode == key->hcode && eq(curr, key)) {
                return from;
            }
        }
        return nullptr;    
    }

    HNode* detach(HNode** from) {
        HNode* node = *from;
        *from = node->next;
        size--;
        return node;
    }

};

const size_t k_resizing_work = 128; // constant work
const size_t k_max_load_factor = 8;

// the real hashtable interface.
// it uses 2 hashtables for progressive resizing.
struct HMap {
    HTab ht1;   // newer
    HTab ht2;   // older
    size_t resizing_pos = 0;

    HNode* lookup(HNode* key, bool (*eq)(HNode*, HNode*)) {
        hm_help_resizing();
        HNode** from = ht1.lookup(key, eq);
        from = from ? from : ht2.lookup(key, eq);
        return from ? *from : nullptr;
    }

    void insert(HNode* node) {
        if (!ht1.tab) {
            ht1 = HTab(4);
        }
        ht1.insert(node);
        if (!ht2.tab) {
            size_t load_factor = ht1.size / (ht1.mask + 1);
            if (load_factor >= k_max_load_factor) {
                hm_start_resizing();
            }
        }
        hm_help_resizing();
    }

    HNode* pop(HNode* key , bool (*eq)(HNode*, HNode*)) {
        hm_help_resizing();
        HNode** from = ht1.lookup(key, eq);
        if (from) {
            return ht1.detach(from);
        }
        from = ht2.lookup(key, eq);
        if (from) {
            return ht2.detach(from);
        }
        return nullptr;
    }

    size_t size() const{
        return ht1.size + ht2.size;
    }

private:
    void hm_help_resizing() {
        size_t nwork = 0;
        while (nwork < k_resizing_work && ht2.size > 0) {
            // scan for nodes from ht2 and move them to ht1
            HNode **from = &ht2.tab[resizing_pos];
            if (!*from) {
                resizing_pos++;
                continue;
            }
            ht1.insert(ht2.detach(from));
            nwork++;
        }
        if (ht2.size == 0 && ht2.tab) {
            // done
            ht2.~HTab();
            ht2 = HTab{};
        }
    }

    void hm_start_resizing() {
        assert(ht2.tab == nullptr);
        ht2 = ht1;
        ht1 = HTab((ht1.mask + 1) * 2);
        resizing_pos = 0;
    }
};