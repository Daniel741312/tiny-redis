#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cmath>

#include "hashtable.h"
#include "common.h"
#include "zset.h"
#include "list.h"
#include "heap.h"
#include "thread_pool.h"

// #define DBG
#include "log.h"

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static uint64_t get_monotonic_usec() {
    timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return uint64_t(tv.tv_sec) * 1000000 + tv.tv_nsec / 1000;
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

const size_t k_max_msg = 4096;
const short PORT = 1234;

struct Conn;

std::unordered_map<int, Conn*> fd2conn;
// The data structure for the key space.
HMap g_map;
DList idle_list;
std::vector<HeapItem> heap;
ThreadPool thread_pool(4);


enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,  // mark the connection for deletion
};


struct Conn {
    int fd = -1;
    uint32_t state = STATE_REQ;     // either STATE_REQ or STATE_RES
    // buffer for reading
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    // buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
    uint64_t idle_start = 0;
    DList idle_list;

    Conn(int connfd) {
        assert(connfd > 0);
        fd = connfd;
        idle_start = get_monotonic_usec();
        LOG_BLUE("fd = %d", fd);
    }

    ~Conn() {
        LOG_RED("fd = %d", fd);
        close(fd);
        dlist_detach(&idle_list);
        fd2conn.erase(fd);  // safe even if `fd` not exists in map
    }

};

static int32_t accept_new_conn(int lfd) {
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(lfd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;  // error
    }

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);
    // creating the struct Conn
    Conn* conn = new Conn(connfd);
    if (!conn) {
        close(connfd);
        return -1;
    }
    dlist_insert_before(&idle_list, &conn->idle_list);
    fd2conn.insert({connfd, conn});
    LOG("fd2conn.size() = %ld", fd2conn.size());

    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

const size_t k_max_args = 1024;

static int32_t parse_req(
    const uint8_t *data, size_t len, std::vector<std::string> &out)
{
    if (len < 4) {
        return -1;
    }
    uint32_t n = 0;
    memcpy(&n, &data[0], 4);
    if (n > k_max_args) {
        return -1;
    }

    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) {
            return -1;
        }
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) {
        return -1;  // trailing garbage
    }
    return 0;
}

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};


enum {
    T_STR = 0,
    T_ZSET = 1,
};

// the structure for the key
struct Entry {
    HNode node;
    std::string key;
    std::string val;
    uint32_t type = 0;
    ZSet* zset = NULL;
    size_t heap_idx = -1;

    Entry(const std::string& k) {
        key = k;
        node.hcode = str_hash(k);
    }
};

static bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

enum {
    ERR_UNKNOWN = 1,
    ERR_2BIG = 2,
    ERR_TYPE = 3,
    ERR_ARG = 4,
};

static void out_nil(std::string &out) {
    out.push_back(SER_NIL);
}

static void out_str(std::string &out, const std::string &val) {
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)val.size();
    out.append((char *)&len, 4);
    out.append(val);
}

static void out_int(std::string &out, int64_t val) {
    out.push_back(SER_INT);
    out.append((char *)&val, 8);
}

static void out_dbl(std::string &out, double val) {
    out.push_back(SER_DBL);
    out.append((char *)&val, 8);
}

static void out_err(std::string &out, int32_t code, const std::string &msg) {
    out.push_back(SER_ERR);
    out.append((char *)&code, 4);
    uint32_t len = (uint32_t)msg.size();
    out.append((char *)&len, 4);
    out.append(msg);
}

static void out_arr(std::string &out, uint32_t n) {
    out.push_back(SER_ARR);
    out.append((char *)&n, 4);
}

static void* begin_arr(std::string &out) {
    out.push_back(SER_ARR);
    out.append("\0\0\0\0", 4);          // filled in end_arr()
    return (void *)(out.size() - 4);    // the `ctx` arg
}

static void end_arr(std::string &out, void *ctx, uint32_t n) {
    size_t pos = (size_t)ctx;
    assert(out[pos - 1] == SER_ARR);
    memcpy(&out[pos], &n, 4);
}

static void do_get(std::vector<std::string> &cmd, std::string& out) {
    Entry key(cmd[1]);

    HNode *node = g_map.lookup(&key.node, &entry_eq);
    if (!node) {
        return out_nil(out);
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->type != T_STR) {
        return out_err(out, ERR_TYPE, "expect string type");
    }
    return out_str(out, ent->val);
}

static void do_set(std::vector<std::string> &cmd, std::string& out) {
    Entry key(cmd[1]);

    HNode *node = g_map.lookup(&key.node, &entry_eq);
    if (node) {
        Entry *ent = container_of(node, Entry, node);
        if (ent->type != T_STR) {
            return out_err(out, ERR_TYPE, "expect string type");
        }
        ent->val.swap(cmd[2]);
    } else {
        Entry *ent = new Entry(key.key);
        ent->val.swap(cmd[2]);
        g_map.insert(&ent->node);
    }
    return out_nil(out);
}

// set or remove the TTL
static void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0 && ent->heap_idx != (size_t)-1) {
        // erase an item from the heap
        // by replacing it with the last item in the array.
        size_t pos = ent->heap_idx;
        heap[pos] = heap.back();
        heap.pop_back();
        if (pos < heap.size()) {
            heap_update(heap.data(), pos, heap.size());
        }
        ent->heap_idx = -1;
    } else if (ttl_ms >= 0) {
        size_t pos = ent->heap_idx;
        if (pos == (size_t)-1) {
            // add an new item to the heap
            HeapItem item;
            item.ref = &ent->heap_idx;
            heap.push_back(item);
            pos = heap.size() - 1;
        }
        heap[pos].val = get_monotonic_usec() + (uint64_t)ttl_ms * 1000;
        heap_update(heap.data(), pos, heap.size());
    }
}

static void entry_destroy(Entry *ent) {
    switch (ent->type) {
    case T_ZSET:
        delete ent->zset;
        break;
    }
    delete ent;
}

static void entry_del_async(void* arg) {
    entry_destroy((Entry*)arg);
}

// dispose the entry after it got detached from the key space
static void entry_del(Entry *ent) {
    entry_set_ttl(ent, -1);

    const size_t k_large_container_size = 10000;
    bool too_big = false;
    switch (ent->type) {
    case T_ZSET:
        too_big = ent->zset->hmap.size() > k_large_container_size;
        break;
    }

    if (too_big) {
        thread_pool.enqueue(&entry_del_async, ent);
    } else {
        entry_destroy(ent);
    }
}

static void do_del(std::vector<std::string> &cmd, std::string& out) {
    Entry key(cmd[1]);

    HNode *node = g_map.pop(&key.node, &entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
    }
    return out_int(out, node ? 1 : 0);
}

static void h_scan(HTab *tab, void (*f)(HNode *, void *), void *arg) {
    if (tab->size == 0) {
        return;
    }
    for (size_t i = 0; i < tab->mask + 1; ++i) {
        HNode *node = tab->tab[i];
        while (node) {
            f(node, arg);
            node = node->next;
        }
    }
}

static void cb_scan(HNode *node, void *arg) {
    std::string &out = *(std::string *)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

static void do_keys(std::vector<std::string> &cmd, std::string &out) {
    (void)cmd;
    out_arr(out, (uint32_t)g_map.size());
    h_scan(&g_map.ht1, &cb_scan, &out);
    h_scan(&g_map.ht2, &cb_scan, &out);
}

static bool str2dbl(const std::string &s, double &out) {
    char *endp = NULL;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !std::isnan(out);
}

static bool str2int(const std::string &s, int64_t &out) {
    char *endp = NULL;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

static void do_expire(std::vector<std::string> &cmd, std::string &out) {
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms)) {
        return out_err(out, ERR_ARG, "expect int64");
    }

    Entry key(cmd[1]);

    HNode* node = g_map.lookup(&key.node, &entry_eq);
    if (node) {
        Entry *ent = container_of(node, Entry, node);
        entry_set_ttl(ent, ttl_ms);
    }
    return out_int(out, node ? 1: 0);
}

static void do_ttl(std::vector<std::string> &cmd, std::string &out) {
    Entry key(cmd[1]);

    HNode *node = g_map.lookup(&key.node, &entry_eq);
    if (!node) {
        return out_int(out, -2);
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->heap_idx == (size_t)-1) {
        return out_int(out, -1);
    }

    uint64_t expire_at = heap[ent->heap_idx].val;
    uint64_t now_us = get_monotonic_usec();
    return out_int(out, expire_at > now_us ? (expire_at - now_us) / 1000 : 0);
}

// zadd zset score name
static void do_zadd(std::vector<std::string> &cmd, std::string &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_ARG, "expect fp number");
    }

    // look up or create the zset
    Entry key(cmd[1]);
    HNode *hnode = g_map.lookup(&key.node, &entry_eq);

    Entry *ent = NULL;
    if (!hnode) {
        ent = new Entry(key.key);
        ent->type = T_ZSET;
        ent->zset = new ZSet();
        g_map.insert(&ent->node);
    } else {
        ent = container_of(hnode, Entry, node);
        if (ent->type != T_ZSET) {
            return out_err(out, ERR_TYPE, "expect zset");
        }
    }

    // add or update the tuple
    bool added = ent->zset->add(cmd[3], score);
    return out_int(out, (int64_t)added);
}

static bool expect_zset(std::string &out, std::string &s, Entry **ent) {
    Entry key(s);
    HNode *hnode = g_map.lookup(&key.node, &entry_eq);
    if (!hnode) {
        out_nil(out);
        return false;
    }

    *ent = container_of(hnode, Entry, node);
    if ((*ent)->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expect zset");
        return false;
    }
    return true;
}

// zrem zset name
static void do_zrem(std::vector<std::string> &cmd, std::string &out) {
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        return;
    }

    ZNode *znode = ent->zset->pop(cmd[2]);
    if (znode) {
        delete znode;
    }
    return out_int(out, znode ? 1 : 0);
}

// zscore zset name
static void do_zscore(std::vector<std::string> &cmd, std::string &out) {
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        return;
    }
    ZNode *znode = ent->zset->look_up(cmd[2]);
    return znode ? out_dbl(out, znode->score) : out_nil(out);
}

// zquery zset score name offset limit
static void do_zquery(std::vector<std::string> &cmd, std::string &out) {
    // parse args
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_ARG, "expect fp number");
    }
    int64_t offset = 0;
    int64_t limit = 0;
    if (!str2int(cmd[4], offset)) {
        return out_err(out, ERR_ARG, "expect int");
    }
    if (!str2int(cmd[5], limit)) {
        return out_err(out, ERR_ARG, "expect int");
    }

    // get the zset
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        if (out[0] == SER_NIL) {
            out.clear();
            out_arr(out, 0);
        }
        return;
    }

    // look up the tuple
    if (limit <= 0) {
        return out_arr(out, 0);
    }
    ZNode *znode = ent->zset->query(score, cmd[3]);
    znode = znode->offset(offset);

    // output
    void *arr = begin_arr(out);
    uint32_t n = 0;
    while (znode && (int64_t)n < limit) {
        out_str(out, znode->name);
        out_dbl(out, znode->score);
        znode = znode->offset(1);
        n += 2;
    }
    end_arr(out, arr, n);
}

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

static void do_request(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys")) {
        do_keys(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        do_del(cmd, out);
    } else if (cmd.size() == 4 && cmd_is(cmd[0], "zadd")) {
        do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zrem")) {
        do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zscore")) {
        do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd_is(cmd[0], "zquery")) {
        do_zquery(cmd, out);
    } else {
        // cmd is not recognized
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

static bool try_one_request(Conn *conn) {
    // try to parse a request from the buffer
    if (conn->rbuf_size < 4) {
        // not enough data in the buffer. Will retry in the next iteration
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) {
        // not enough data in the buffer. Will retry in the next iteration
        return false;
    }

    // parse the request
    std::vector<std::string> cmd;
    if (0 != parse_req(&conn->rbuf[4], len, cmd)) {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    // got one request, generate the response.
    std::string out;
    do_request(cmd, out);

    // pack the response into the buffer
    if (4 + out.size() > k_max_msg) {
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }
    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen;

    // remove the request from the buffer.
    // note: frequent memmove is inefficient.
    // note: need better handling for production code.
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    // change state
    conn->state = STATE_RES;
    state_res(conn);

    // continue the outer loop if the request was fully processed
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    // try to fill the buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // Try to process requests one by one.
    // Why is there a loop? Please read the explanation of "pipelining".
    while (try_one_request(conn)) {}
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        // response was fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    // still got some data in wbuf, could try to write again
    return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static void connection_io(Conn *conn) {
    conn->idle_start = get_monotonic_usec();
    dlist_detach(&conn->idle_list);
    dlist_insert_before(&idle_list, &conn->idle_list);

    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);  // not expected
    }
}

const uint64_t k_idle_timeout_ms = 5 * 1000;

static uint32_t next_timer_ms() {
    uint64_t now_us = get_monotonic_usec();
    uint64_t next_us = (uint64_t)-1;
    
    if (!dlist_empty(&idle_list)) {
        Conn *next = container_of(idle_list.next, Conn, idle_list);
        next_us = next->idle_start + k_idle_timeout_ms * 1000;
    }

    if (!heap.empty() &&  heap[0].val < next_us) {
        next_us = heap[0].val;
    }

    if (next_us == (uint64_t)-1) {
        return 10000;
    }

    if (next_us <= now_us) {
        // missed?
        return 0;
    }

    return (uint32_t)((next_us - now_us) / 1000);
}

static bool hnode_same(HNode *lhs, HNode *rhs) {
    return lhs == rhs;
}

static void process_timers() {
    uint64_t now_us = get_monotonic_usec() + 1000;

    while (!dlist_empty(&idle_list)) {
        Conn *next = container_of(idle_list.next, Conn, idle_list);
        uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
        if (next_us >= now_us + 1000) {
            // not ready, the extra 1000us is for the ms resolution of poll()
            break;
        }

        printf("removing idle connection: %d\n", next->fd);
        delete next;
    }

    // TTL timers
    const size_t k_max_works = 2000;
    size_t nworks = 0;
    while (!heap.empty() && heap[0].val < now_us) {
        Entry *ent = container_of(heap[0].ref, Entry, heap_idx);
        HNode *node = g_map.pop(&ent->node, &hnode_same);
        assert(node == &ent->node);
        entry_del(ent);
        if (nworks++ >= k_max_works) {
            // don't stall the server if too many keys are expiring at once
            break;
        }
    }
}

int main() {
    dlist_init(&idle_list);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // the event loop
    std::vector<struct pollfd> poll_args;
    while (true) {
        // prepare the arguments of the poll()
        poll_args.clear();
        // for convenience, the listening fd is put in the first position
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        // connection fds
        for (auto& fd_conn: fd2conn) {
            Conn* conn = fd_conn.second;
            if (!conn) {
                continue;
            }
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }

        // poll for active fds
        int timeout_ms = (int)next_timer_ms();
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (rv < 0) {
            die("poll");
        }

        // process active connections
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // client closed normally, or something bad happened.
                    // destroy this connection
                    delete conn;
                }
            }
        }

        process_timers();

        // try to accept a new connection if the listening fd is active
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd);
        }
    }

    return 0;
}
