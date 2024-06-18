#include <assert.h>
#include <string.h>
#include <stdlib.h>
// proj
#include "zset.h"
#include "common.h"

// compare by the (score, name) tuple
static bool zless(AVLNode *lhs, double score, const std::string& name) {
    ZNode* zl = container_of(lhs, ZNode, tree);
    if (zl->score != score) {
        return zl->score < score;
    }
    return zl->name < name;
}

static bool zless(AVLNode *lhs, AVLNode *rhs) {
    ZNode* zr = container_of(rhs, ZNode, tree);
    return zless(lhs, zr->score, zr->name);
}

// ZSet
// insert into the AVL tree
void ZSet::tree_add(ZNode *node) {
    AVLNode* cur = nullptr;            // current node
    AVLNode** from = &tree;   // the incoming pointer to the next node
    while (*from) {                 // tree search
        cur = *from;
        from = zless(&node->tree, cur) ? &cur->left : &cur->right;
    }
    *from = &node->tree;            // attach the new node
    node->tree.parent = cur;
    tree = avl_fix(&node->tree);
}

// update the score of an existing node (AVL tree reinsertion)
void ZSet::update(ZNode *node, double score) {
    if (node->score == score) {
        return;
    }
    tree = avl_del(&node->tree);
    node->score = score;
    node->tree.init();
    tree_add(node);
}

// add a new (score, name) tuple, or update the score of the existing tuple
bool ZSet::add(const std::string& name, double score) {
    auto node = look_up(name);
    if (node) {
        update(node, score);
        return false;
    }
        node = new ZNode(name, score);
        hmap.insert(&node->hmap);
        tree_add(node);
        return true;
}

// a helper structure for the hashtable lookup
struct HKey {
    HNode node;
    std::string name;

    HKey(const std::string& name) {
        this->name = name;
        node.hcode = str_hash(name);
    }
};

static bool hcmp(HNode *node, HNode *key) {
    ZNode *znode = container_of(node, ZNode, hmap);
    HKey *hkey = container_of(key, HKey, node);
    return hkey->name == znode->name;
}

// lookup by name
ZNode* ZSet::look_up(const std::string& name) {
    HKey key(name);
    HNode* found = hmap.lookup(&key.node, &hcmp);
    return found ? container_of(found, ZNode, hmap) : nullptr;
}

// deletion by name
ZNode* ZSet::pop(const std::string& name) {
    HKey key(name);
    HNode *found = hmap.pop(&key.node, &hcmp);
    if (!found) {
        return nullptr;
    }

    ZNode *node = container_of(found, ZNode, hmap);
    tree = avl_del(&node->tree);
    return node;
}

// find the (score, name) tuple that is greater or equal to the argument.
ZNode* ZSet::query(double score, const std::string& name) {
    AVLNode *found = NULL;
    AVLNode *cur = tree;
    while (cur) {
        if (zless(cur, score, name)) {
            cur = cur->right;
        } else {
            found = cur;    // candidate
            cur = cur->left;
        }
    }
    return found ? container_of(found, ZNode, tree) : NULL;
}


static void tree_dispose(AVLNode *node) {
    if (!node) {
        return;
    }
    tree_dispose(node->left);
    tree_dispose(node->right);
    free(container_of(node, ZNode, tree));
}

ZSet::~ZSet() {
    tree_dispose(tree);
}
