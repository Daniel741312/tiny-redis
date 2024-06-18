#pragma once

#include <stddef.h>
#include <stdint.h>

#include "avl.h"
#include "hashtable.h"
#include "common.h"
#include <string>
#include <cstdlib>
#include <cstring>

struct ZNode;

struct ZSet {
    AVLNode* tree = NULL;
    HMap hmap;

public:
    bool add(const std::string& name, double score);
    ZNode* look_up(const std::string& name);
    ZNode* pop(const std::string& name);
    ZNode* query(double score, const std::string& name);
    ~ZSet();

private:
    void tree_add(ZNode* node);
    void update(ZNode* node, double score);

};

struct ZNode {
    AVLNode tree;
    HNode hmap;
    double score = 0;
    std::string name;

    ZNode(std::string name, double score) {
        this->name = name;
        hmap.hcode = str_hash(name);
        this->score = score;
    }

    ZNode* offset(int64_t offset) {
        auto tnode = avl_offset(&tree, offset);
        return tnode ? container_of(tnode, ZNode, tree) : nullptr;
    }
};
