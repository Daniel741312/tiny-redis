#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// hashtable node, should be embedded into the payload
struct HNode {
    HNode* next;
    uint64_t hcode;

    HNode(): next(nullptr), hcode(0) {}
    HNode(HNode* n, uint64_t hc): next(n), hcode(hc) {}
};

// a simple fixed-sized hashtable
struct HTab {
    HNode **tab;
    size_t mask;
    size_t size;

    HTab(): tab(nullptr), mask(0), size(0) {}
    // HTab(size_t n); 
    void resize(size_t n);
    ~HTab() {
        free(tab);
        tab = nullptr;
    }

    void insert(HNode* node);
    HNode** lookup(HNode* key, bool (*eq)(HNode*, HNode*));
    HNode* detach(HNode** from) ;
};

// the real hashtable interface.
// it uses 2 hashtables for progressive resizing.
struct HMap {
    HTab ht1;   // newer
    HTab ht2;   // older
    size_t resizing_pos = 0;

    HNode* lookup(HNode* key, bool (*eq)(HNode*, HNode*));
    void insert(HNode* node);
    HNode* pop(HNode* key , bool (*eq)(HNode*, HNode*));
    size_t size() const;

private:
    void hm_help_resizing();
    void hm_start_resizing();
};