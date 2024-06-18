#include "avl.h"

void AVLNode::update() {
    uint32_t l = DEPTH(left);
    uint32_t r = DEPTH(right);
    depth = 1 + MAX(l, r);
    cnt = 1 + CNT(left)+ CNT(right);
}

AVLNode *AVLNode::rot_left() {
    auto new_node = right;
    if (new_node->left) {
        new_node->left->parent = this;
    }
    right = new_node->left;
    new_node->left = this;
    new_node->parent = this->parent;
    this->parent = new_node;
    this->update();
    new_node->update();
    return new_node;
}

AVLNode *AVLNode::rot_right() {
    auto new_node = left;
    if (new_node->right) {
        new_node->right->parent = this;
    }
    left = new_node->right;
    new_node->right = this;
    new_node->parent = parent;
    parent = new_node;
    this->update();
    new_node->update();
    return new_node;
}

AVLNode *AVLNode::fix_left() {
    if (DEPTH(left->left) < DEPTH(left->right)) {
        left = left->rot_left();
    }
    return this->rot_right();
}

AVLNode *AVLNode::fix_right() {
    if (DEPTH(right->right) < DEPTH(right->left)) {
        right = right->rot_right();
    }
    return this->rot_left();
}

// fix imbalanced nodes and maintain invariants until the root is reached
AVLNode *avl_fix(AVLNode *node) {
    while (true) {
        node->update();
        uint32_t l = DEPTH(node->left);
        uint32_t r = DEPTH(node->right);
        AVLNode **from = NULL;
        if (node->parent) {
            from = (node->parent->left == node) ? &node->parent->left
                                                : &node->parent->right;
        }
        if (l == r + 2) {
            node = node->fix_left();
        } else if (l + 2 == r) {
            node = node->fix_right();
        }
        if (!from) {
            return node;
        }
        *from = node;
        node = node->parent;
    }
}

// detach a node and returns the new root of the tree
AVLNode *avl_del(AVLNode *node) {
    if (node->right == NULL) {
        // no right subtree, replace the node with the left subtree
        // link the left subtree to the parent
        AVLNode *parent = node->parent;
        if (node->left) {
            node->left->parent = parent;
        }
        if (parent) {
            // attach the left subtree to the parent
            (parent->left == node ? parent->left : parent->right) = node->left;
            return avl_fix(parent);
        } else {
            // removing root?
            return node->left;
        }
    } else {
        // swap the node with its next sibling
        AVLNode *victim = node->right;
        while (victim->left) {
            victim = victim->left;
        }
        AVLNode *root = avl_del(victim);

        *victim = *node;
        if (victim->left) {
            victim->left->parent = victim;
        }
        if (victim->right) {
            victim->right->parent = victim;
        }
        AVLNode *parent = node->parent;
        if (parent) {
            (parent->left == node ? parent->left : parent->right) = victim;
            return root;
        } else {
            // removing root?
            return victim;
        }
    }
}

// offset into the succeeding or preceding node.
// note: the worst-case is O(log(n)) regardless of how long the offset is.
AVLNode *avl_offset(AVLNode *node, int64_t offset) {
    int64_t pos = 0;    // relative to the starting node
    while (offset != pos) {
        if (pos < offset && pos + CNT(node->right) >= offset) {
            // the target is inside the right subtree
            node = node->right;
            pos += CNT(node->left) + 1;
        } else if (pos > offset && pos - CNT(node->left) <= offset) {
            // the target is inside the left subtree
            node = node->left;
            pos -= CNT(node->right) + 1;
        } else {
            // go to the parent
            AVLNode *parent = node->parent;
            if (!parent) {
                return NULL;
            }
            if (parent->right == node) {
                pos -= CNT(node->left) + 1;
            } else {
                pos += CNT(node->right) + 1;
            }
            node = parent;
        }
    }
    return node;
}
