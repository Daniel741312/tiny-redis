#include "hashtable.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define DBG
#include "log.h"

const size_t k_resizing_work = 128;  // constant work
const size_t k_max_load_factor = 8;

void HTab::resize(size_t n) {
    // n must be a power of 2
    assert(n > 0 && ((n - 1) & n) == 0);
    tab = (HNode **)calloc(sizeof(HNode *), n);
    memset(tab, 0, sizeof(HNode*) * n);
    mask = n - 1;
    size = 0;
}

void HTab::insert(HNode* node) {
    size_t pos = node->hcode & mask;
    HNode* first = tab[pos];
    node->next = first;
    tab[pos] = node;
    size++;
}

HNode** HTab::lookup(HNode* key, bool (*eq)(HNode*, HNode*)) {
    if (!tab) {
        return nullptr;
    }

    auto pos = key->hcode & mask;

    HNode** from = &tab[pos];
    for (HNode* curr; (curr = *from) != nullptr; from = &(curr->next)) {
        if (curr->hcode == key->hcode && eq(curr, key)) {
            return from;
        }
    }
    return nullptr;
}

HNode* HTab::detach(HNode** from) {
    HNode* node = *from;
    *from = node->next;
    size--;
    return node;
}

HNode* HMap::lookup(HNode* key, bool (*eq)(HNode*, HNode*)) {
    hm_help_resizing();
    HNode** from = ht1.lookup(key, eq);
    from = from ? from : ht2.lookup(key, eq);
    return from ? *from : nullptr;
}

void HMap::insert(HNode* node) {
    if (!ht1.tab) {
        ht1.resize(4);
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

HNode* HMap::pop(HNode* key, bool (*eq)(HNode*, HNode*)) {
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

void HMap::hm_help_resizing() {
    size_t nwork = 0;
    while (nwork < k_resizing_work && ht2.size > 0) {
        // scan for nodes from ht2 and move them to ht1
        HNode** from = &ht2.tab[resizing_pos];
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

void HMap::hm_start_resizing() {
    assert(ht2.tab == nullptr);
    ht2 = ht1;
    ht1.resize((ht1.mask + 1) * 2);
    resizing_pos = 0;
}