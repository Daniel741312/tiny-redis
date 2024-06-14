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
#include <vector>
#include <unordered_map>
#include <sys/epoll.h>
#include <string>
#include <map>

#define DBG
#include "log.h"

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
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
const int EPOLL_SIZE = 1024;

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

    Conn() {}
    Conn(int connfd): fd(connfd) {
        Conn();
    }
};

std::unordered_map<int, Conn*> fd2conn;

static int32_t accept_new_conn(int lfd) {
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(lfd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;  // error
    }
    LOG("connfd = %d", connfd);

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);
    // creating the struct Conn
    Conn* conn = new Conn(connfd);
    if (!conn) {
        close(connfd);
        return -1;
    }
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

// The data structure for the key space. This is just a placeholder
// until we implement a hashtable in the next chapter.
static std::map<std::string, std::string> g_map;

static uint32_t do_get(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }
    std::string &val = g_map[cmd[1]];
    LOG("k = %s", cmd[1].c_str());
    assert(val.size() <= k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = (uint32_t)val.size();
    return RES_OK;
}

static uint32_t do_set(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map[cmd[1]] = cmd[2];
    LOG("k = %s, v = %s", cmd[1].c_str(), cmd[2].c_str());
    return RES_OK;
}

static uint32_t do_del(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map.erase(cmd[1]);
    LOG("k = %s", cmd[1].c_str());
    return RES_OK;
}

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

static int32_t do_request(
    const uint8_t *req, uint32_t reqlen,
    uint32_t *rescode, uint8_t *res, uint32_t *reslen)
{
    std::vector<std::string> cmd;
    if (0 != parse_req(req, reqlen, cmd)) {
        msg("bad req");
        return -1;
    }
    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
    } else {
        // cmd is not recognized
        *rescode = RES_ERR;
        const char *msg = "Unknown cmd";
        strcpy((char *)res, msg);
        *reslen = strlen(msg);
        return 0;
    }
    return 0;
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

    // got one request, do something with it
    // printf("client says: %.*s\n", len, &conn->rbuf[4]);
    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(
        &conn->rbuf[4], len,
        &rescode, &conn->wbuf[4 + 4], &wlen
    );
    if (err) {
        conn->state = STATE_END;
        return false;
    }
    wlen += 4;

    // generating echoing response
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], &rescode, 4);
    conn->wbuf_size = 4 + len;
    LOG("wlen = %u", wlen);
    LOG("rescode = %u", rescode);

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
    LOG("read rv = %ld", rv);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        LOG("err_msg: %s", strerror(errno));
        return false;
    }
    if (rv < 0 && errno == ECONNRESET) {
        // printf("line: %d, errno = %d, msg = %s\n", __LINE__, errno, strerror(errno));
        LOG("err_msg: %s", strerror(errno));
        conn->state = STATE_END;
        return false;
    }
    if (rv < 0) {
        msg("read() error");
        LOG("errno = %d, msg = %s", errno, strerror(errno));
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
        LOG("write rv = %ld", rv);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        LOG("err_msg: %s", strerror(errno));
        return false;
    }
    LOG("err_msg: %s", strerror(errno));
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        // response was fully sent, change state back
        LOG("response was fully sent, change state back");
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
    LOG("conn->state = %d", conn->state);
    if (conn->state == STATE_REQ) {
        LOG("before state_req");
        state_req(conn);
        LOG("after state_req");
    } else if (conn->state == STATE_RES) {
        LOG("before state_res");
        state_res(conn);
        LOG("after state_res");
    } else {
        assert(0);  // not expected
    }
}

int main() {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        die("socket()");
    } else {
        LOG("lfd = %d", lfd);
    }

    int val = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(PORT);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(lfd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // listen
    rv = listen(lfd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // set the listen fd to nonblocking mode
    fd_set_nb(lfd);

    // the event loop
    std::vector<struct epoll_event> epoll_events{EPOLL_SIZE};
    int epfd = epoll_create(EPOLL_SIZE);
    if (epfd == -1) {
        die("epoll_create()");
    }

    struct epoll_event lev = {
        .events = EPOLLIN,
        .data = {.fd = lfd}
    };

    rv = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev);
    if (rv) {
        die("epoll_ctl() add lfd");
    }

    while (true) {
        // prepare the arguments of the poll()
        epoll_events.clear();
        // for convenience, the listening fd is put in the first position
        // connection fds
        for (auto fd_conn : fd2conn) {
            auto conn = fd_conn.second;
            if (!conn) {
                continue;
            }
            LOG("conn->state = %d", conn->state);
            struct epoll_event ev = {
                .events = (conn->state == STATE_REQ) ? EPOLLIN : EPOLLOUT,
                .data = {.fd = conn->fd}
            };
            // ev.events |= (EPOLLERR);
            rv = epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &ev);
            if (rv) {
                if (errno == 17) {
                    // printf("repeated add\n");
                    rv = epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
                    LOG("epoll_ctl MOD %d, from EPOLLIN | EPOLLOUT to 0x%x", conn->fd, ev.events);

                    if (rv) {
                        die("epoll_ctl MOD");
                    }
                } else {
                    die(strerror(errno));
                }
            } else {
                LOG("epoll_ctl ADD %d, event = 0x%x", conn->fd, ev.events);
            }
        }

        // poll for active fds
        // the timeout argument doesn't matter here
        rv = epoll_wait(epfd, epoll_events.data(), EPOLL_SIZE, 1000); 
        if (rv < 0) {
            die("epoll_wait");
        } else {
            LOG("epoll_wait returns rv = %d", rv);
        }

        // process active connections
        for (size_t i = 0; i < rv; ++i) {
            if (epoll_events[i].data.fd == lfd) {
                (void)accept_new_conn(lfd);
                continue;
            }
            if (epoll_events[i].events & EPOLLIN ||  epoll_events[i].events & EPOLLOUT) {
                LOG("epoll_events[i].events = 0x%x, fd = %d", epoll_events[i].events, epoll_events[i].data.fd);
                Conn *conn = fd2conn[epoll_events[i].data.fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // client closed normally, or something bad happened.
                    // destroy this connection
                    fd2conn[conn->fd] = NULL;
                    rv = epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                    if (rv) {
                        die("epoll_ctl DEL");
                    }
                    (void)close(conn->fd);
                    fd2conn.erase(conn->fd);
                    LOG("epoll_ctl DEL, conn->fd = %d, fd2conn.size() = %ld", conn->fd, fd2conn.size());
                    free(conn);
                }
            }
        }
    }

    return 0;
}
