#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAX(lhs, rhs) \
    lhs < rhs ? rhs : lhs
#define MIN(lhs, rhs) \
    lhs > rhs ? rhs : lhs

#define DEPTH(node) (node ? node->depth : 0)

#define CNT(node) (node ? node->cnt : 0)

struct AVLNode {
    uint32_t depth;
    uint32_t cnt;
    AVLNode *left;
    AVLNode *right;
    AVLNode *parent;

    AVLNode()
        : depth(1), cnt(1), left(nullptr), right(nullptr), parent(nullptr) {}
    
    void init() {
        depth = cnt = 1;
        left = right = parent = nullptr;
    }

    void update();

    AVLNode *rot_left();
    AVLNode *rot_right();

    AVLNode *fix_left();
    AVLNode *fix_right();
};

AVLNode *avl_fix(AVLNode *node);
AVLNode *avl_del(AVLNode *node);
AVLNode *avl_offset(AVLNode *node, int64_t offset);
