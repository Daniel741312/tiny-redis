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
const int EPOLL_SIZE = 512;

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

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);
    // creating the struct Conn
    Conn* conn = new Conn(connfd);
    if (!conn) {
        close(connfd);
        return -1;
    }
    fd2conn.insert({connfd, conn});
    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

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
    printf("client says: %.*s\n", len, &conn->rbuf[4]);

    // generating echoing response
    memcpy(&conn->wbuf[0], &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_size = 4 + len;

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
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);  // not expected
    }
}

int main() {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        die("socket()");
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
        .events = EPOLLIN | EPOLLET,
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
            struct epoll_event ev = {
                .events = (conn->state == STATE_REQ) ? EPOLLIN : EPOLLOUT,
                .data = {.fd = conn->fd}
            };
            ev.events |= (EPOLLET | EPOLLERR);
            rv = epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &ev);
            if (rv) {
                if (errno == 17) {
                    // printf("repeated add\n");
                } else {
                    die(strerror(errno));
                }
            }
        }

        // poll for active fds
        // the timeout argument doesn't matter here
        rv = epoll_wait(epfd, epoll_events.data(), EPOLL_SIZE, 1000); 
        if (rv < 0) {
            die("epoll_wait");
        }

        // process active connections
        for (size_t i = 0; i < rv; ++i) {
            if (epoll_events[i].data.fd == lfd) {
                (void)accept_new_conn(lfd);
                continue;
            }
            if (epoll_events[i].events & EPOLLIN ||  epoll_events[i].events & EPOLLOUT) {
                Conn *conn = fd2conn[epoll_events[i].data.fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // client closed normally, or something bad happened.
                    // destroy this connection
                    fd2conn[conn->fd] = NULL;
                    rv = epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                    if (rv) {
                        die("epoll_ctl del");
                    }
                    (void)close(conn->fd);
                    fd2conn.erase(conn->fd);
                    free(conn);
                }
            }
        }
    }

    return 0;
}