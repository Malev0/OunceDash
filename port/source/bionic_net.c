// Linux/bionic sockets on the Switch BSD service.
//
// Differences handled here: sockaddr has sa_len on the BSD side, AF_INET6
// is 10 vs 28, SOL_SOCKET/SO_* numbering, SOCK_NONBLOCK, ioctl numbers,
// addrinfo flags and EAI_* codes, and errno values (curl's non-blocking
// connect relies on EINPROGRESS).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "port.h"

// ---------------------------------------------------------------- errno

static int errno_to_linux(int e)
{
    switch (e) {
    case EAFNOSUPPORT: return 97;
    case ENOTSOCK: return 88;
    case ENOPROTOOPT: return 92;
    case EADDRINUSE: return 98;
    case ECONNABORTED: return 103;
    case ENETUNREACH: return 101;
    case ENETDOWN: return 100;
    case ETIMEDOUT: return 110;
    case EHOSTUNREACH: return 113;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTONOSUPPORT: return 93;
    case EADDRNOTAVAIL: return 99;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    default: return e; // EAGAIN, EINTR, EBADF, ECONNRESET, ECONNREFUSED, EPIPE ... match
    }
}

static inline int fix(int rc)
{
    bionic_thread_checkpoint();
    if (rc < 0) errno = errno_to_linux(errno);
    return rc;
}
static inline ssize_t fixs(ssize_t rc)
{
    bionic_thread_checkpoint();
    if (rc < 0) errno = errno_to_linux(errno);
    return rc;
}

// ---------------------------------------------------------------- sockaddr

#define L_AF_INET6 10

static int fam_to_bsd(int f) { return f == L_AF_INET6 ? AF_INET6 : f; }
static int fam_to_linux(int f) { return f == AF_INET6 ? L_AF_INET6 : f; }

static socklen_t sa_to_bsd(const void *lin, socklen_t len, struct sockaddr_storage *out)
{
    if (!lin) return 0;
    if (len > sizeof(*out)) len = sizeof(*out);
    memset(out, 0, sizeof(*out));
    memcpy(out, lin, len);
    uint16_t fam = *(const uint16_t *)lin;
    uint8_t *b = (uint8_t *)out;
    b[0] = (uint8_t)len;
    b[1] = (uint8_t)fam_to_bsd(fam);
    return len;
}

static void sa_to_linux(const struct sockaddr_storage *bsd, socklen_t bsd_len, void *lin, socklen_t *lin_len)
{
    if (!lin || !lin_len) return;
    uint8_t tmp[sizeof(struct sockaddr_storage)];
    memcpy(tmp, bsd, bsd_len);
    *(uint16_t *)tmp = (uint16_t)fam_to_linux(((const uint8_t *)bsd)[1]);
    socklen_t n = bsd_len < *lin_len ? bsd_len : *lin_len;
    memcpy(lin, tmp, n);
    *lin_len = bsd_len;
}

// ---------------------------------------------------------------- socket calls

int b_socket(int domain, int type, int proto)
{
    bool nonblock = type & 0x800;
    type &= ~(0x800 | 0x80000);
    int fd = socket(fam_to_bsd(domain), type, proto);
    if (fd >= 0 && nonblock)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fix(fd);
}

int b_connect(int fd, const void *sa, socklen_t len)
{
    struct sockaddr_storage b;
    socklen_t bl = sa_to_bsd(sa, len, &b);
    int rc = fix(connect(fd, (struct sockaddr *)&b, bl));
    if (rc < 0 && errno != 115 /* EINPROGRESS */) {
        char ip[64] = "?";
        if (((uint8_t *)&b)[1] == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&b)->sin_addr, ip, sizeof(ip));
        LOG("net: connect(%s) failed, errno %d\n", ip, errno);
    }
    return rc;
}

int b_bind(int fd, const void *sa, socklen_t len)
{
    struct sockaddr_storage b;
    socklen_t bl = sa_to_bsd(sa, len, &b);
    return fix(bind(fd, (struct sockaddr *)&b, bl));
}

int b_listen(int fd, int backlog) { return fix(listen(fd, backlog)); }

int b_accept(int fd, void *sa, socklen_t *len)
{
    struct sockaddr_storage b;
    socklen_t bl = sizeof(b);
    int rc = accept(fd, (struct sockaddr *)&b, &bl);
    if (rc >= 0) sa_to_linux(&b, bl, sa, len);
    return fix(rc);
}

static int msg_flags(int f)
{
    int o = f & 0x2; // MSG_PEEK
    if (f & 0x1) o |= MSG_OOB;
    if (f & 0x40) o |= MSG_DONTWAIT;
    if (f & 0x100) o |= MSG_WAITALL;
    return o; // MSG_NOSIGNAL (0x4000) dropped: no signals here
}

ssize_t b_send(int fd, const void *buf, size_t n, int flags) { return fixs(send(fd, buf, n, msg_flags(flags))); }
ssize_t b_recv(int fd, void *buf, size_t n, int flags) { return fixs(recv(fd, buf, n, msg_flags(flags))); }

ssize_t b_sendto(int fd, const void *buf, size_t n, int flags, const void *sa, socklen_t len)
{
    struct sockaddr_storage b;
    socklen_t bl = sa_to_bsd(sa, len, &b);
    return fixs(sendto(fd, buf, n, msg_flags(flags), sa ? (struct sockaddr *)&b : NULL, bl));
}

ssize_t b_recvfrom(int fd, void *buf, size_t n, int flags, void *sa, socklen_t *len)
{
    struct sockaddr_storage b;
    socklen_t bl = sizeof(b);
    ssize_t rc = recvfrom(fd, buf, n, msg_flags(flags), (struct sockaddr *)&b, &bl);
    if (rc >= 0 && sa) sa_to_linux(&b, bl, sa, len);
    return fixs(rc);
}

int b_shutdown(int fd, int how) { return fix(shutdown(fd, how)); }

int b_getpeername(int fd, void *sa, socklen_t *len)
{
    struct sockaddr_storage b;
    socklen_t bl = sizeof(b);
    int rc = getpeername(fd, (struct sockaddr *)&b, &bl);
    if (rc == 0) sa_to_linux(&b, bl, sa, len);
    return fix(rc);
}

int b_getsockname(int fd, void *sa, socklen_t *len)
{
    struct sockaddr_storage b;
    socklen_t bl = sizeof(b);
    int rc = getsockname(fd, (struct sockaddr *)&b, &bl);
    if (rc == 0) sa_to_linux(&b, bl, sa, len);
    return fix(rc);
}

static bool sockopt_to_bsd(int level, int opt, int *blevel, int *bopt)
{
    if (level == 1 /* SOL_SOCKET */) {
        *blevel = SOL_SOCKET;
        switch (opt) {
        case 2: *bopt = SO_REUSEADDR; return true;
        case 3: *bopt = SO_TYPE; return true;
        case 4: *bopt = SO_ERROR; return true;
        case 6: *bopt = SO_BROADCAST; return true;
        case 7: *bopt = SO_SNDBUF; return true;
        case 8: *bopt = SO_RCVBUF; return true;
        case 9: *bopt = SO_KEEPALIVE; return true;
        case 13: *bopt = SO_LINGER; return true;
        case 20: *bopt = SO_RCVTIMEO; return true;
        case 21: *bopt = SO_SNDTIMEO; return true;
        default: return false;
        }
    }
    if (level == IPPROTO_TCP) {
        *blevel = IPPROTO_TCP;
        if (opt == 1) { *bopt = TCP_NODELAY; return true; }
        return false; // TCP_KEEPIDLE & co: not supported, ignored
    }
    if (level == IPPROTO_IP) { *blevel = level; *bopt = opt; return true; }
    return false;
}

int b_setsockopt(int fd, int level, int opt, const void *val, socklen_t len)
{
    int bl, bo;
    if (!sockopt_to_bsd(level, opt, &bl, &bo))
        return 0;
    int rc = setsockopt(fd, bl, bo, val, len);
    if (rc < 0) {
        LOG("setsockopt(%d,%d) -> errno %d (ignored)\n", level, opt, errno);
        return 0;
    }
    return 0;
}

int b_getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
    int bl, bo;
    if (!sockopt_to_bsd(level, opt, &bl, &bo)) {
        if (val && len && *len >= 4) *(int *)val = 0;
        return 0;
    }
    int rc = getsockopt(fd, bl, bo, val, len);
    if (rc == 0 && bo == SO_ERROR && val)
        *(int *)val = errno_to_linux(*(int *)val);
    return fix(rc);
}

bool rand_fd_is(int fd);

int b_poll(struct pollfd *fds, unsigned n, int timeout)
{
    // the fake random devices are always readable
    int ready = 0;
    bool any_real = false;
    for (unsigned i = 0; i < n; i++) {
        if (rand_fd_is(fds[i].fd)) {
            fds[i].revents = fds[i].events & (POLLIN | POLLRDNORM);
            if (fds[i].revents) ready++;
        } else {
            any_real = true;
        }
    }
    if (!ready) {
        bionic_thread_checkpoint();
        return fix(poll(fds, n, timeout));
    }
    if (!any_real) return ready;
    // mixed: report the random devices plus whatever real fds are ready now
    struct pollfd tmp[64];
    unsigned m = 0;
    int map[64];
    for (unsigned i = 0; i < n && m < 64; i++)
        if (!rand_fd_is(fds[i].fd)) { tmp[m] = fds[i]; map[m] = i; m++; }
    int rc = poll(tmp, m, 0);
    if (rc > 0) {
        for (unsigned k = 0; k < m; k++) fds[map[k]].revents = tmp[k].revents;
        ready += rc;
    }
    return ready;
}

int b_select(int n, void *r, void *w, void *e, void *t)
{
    (void)n; (void)r; (void)w; (void)e; (void)t;
    errno = 38; // ENOSYS: only fmod's unused net streaming would call this
    return -1;
}

int b_ioctl(int fd, unsigned long req, void *arg)
{
    switch (req) {
    case 0x5421: /* FIONBIO */ {
        int on = arg ? *(int *)arg : 0;
        int fl = fcntl(fd, F_GETFL);
        return fix(fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)));
    }
    case 0x541b: /* FIONREAD */
        return fix(ioctl(fd, FIONREAD, arg));
    default:
        errno = 22;
        return -1;
    }
}

int net_fcntl(int fd, int cmd, long arg, bool *handled)
{
    (void)fd; (void)cmd; (void)arg;
    *handled = false;
    return 0;
}

// ---------------------------------------------------------------- name resolution

typedef struct baddrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct baddrinfo *ai_next;
} baddrinfo;

static int eai_to_linux(int e)
{
    switch (e) {
    case 0: return 0;
    case EAI_AGAIN: return -3;
    case EAI_BADFLAGS: return -1;
    case EAI_FAIL: return -4;
    case EAI_FAMILY: return -6;
    case EAI_MEMORY: return -10;
    case EAI_NONAME: return -2;
    case EAI_SERVICE: return -8;
    case EAI_SOCKTYPE: return -7;
    case EAI_SYSTEM: return -11;
    case EAI_OVERFLOW: return -12;
#ifdef EAI_NODATA
    case EAI_NODATA: return -5;
#endif
    default: return -4;
    }
}

int b_getaddrinfo(const char *node, const char *service, const baddrinfo *hints, baddrinfo **res)
{
    struct addrinfo h, *r = NULL;
    memset(&h, 0, sizeof(h));
    if (hints) {
        int f = hints->ai_flags;
        h.ai_flags = f & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
        if (f & 0x400) h.ai_flags |= AI_NUMERICSERV;
        h.ai_family = fam_to_bsd(hints->ai_family);
        h.ai_socktype = hints->ai_socktype;
        h.ai_protocol = hints->ai_protocol;
    }
    int rc = getaddrinfo(node, service, hints ? &h : NULL, &r);
    if (rc != 0) {
        LOG("getaddrinfo(%s) failed: %d\n", node ? node : "(null)", rc);
        return eai_to_linux(rc);
    }
    baddrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *p = r; p; p = p->ai_next) {
        size_t cn = p->ai_canonname ? strlen(p->ai_canonname) + 1 : 0;
        baddrinfo *b = calloc(1, sizeof(*b) + sizeof(struct sockaddr_storage) + cn);
        b->ai_flags = p->ai_flags;
        b->ai_family = fam_to_linux(p->ai_family);
        b->ai_socktype = p->ai_socktype;
        b->ai_protocol = p->ai_protocol;
        b->ai_addr = (uint8_t *)(b + 1);
        socklen_t len = sizeof(struct sockaddr_storage);
        sa_to_linux((struct sockaddr_storage *)p->ai_addr, p->ai_addrlen, b->ai_addr, &len);
        b->ai_addrlen = p->ai_addrlen;
        if (cn) {
            b->ai_canonname = (char *)b->ai_addr + sizeof(struct sockaddr_storage);
            memcpy(b->ai_canonname, p->ai_canonname, cn);
        }
        *tail = b;
        tail = &b->ai_next;
    }
    freeaddrinfo(r);
    *res = head;
    if (head && head->ai_family == AF_INET) {
        char ip[64];
        inet_ntop(AF_INET, (uint8_t *)head->ai_addr + 4, ip, sizeof(ip));
        LOG("net: %s -> %s\n", node ? node : "(null)", ip);
    }
    return 0;
}

void b_freeaddrinfo(baddrinfo *a)
{
    while (a) {
        baddrinfo *n = a->ai_next;
        free(a);
        a = n;
    }
}

const char *b_gai_strerror(int e)
{
    switch (e) {
    case -2: return "Name or service not known";
    case -3: return "Temporary failure in name resolution";
    case -4: return "Non-recoverable failure in name resolution";
    default: return "getaddrinfo error";
    }
}

int b_getnameinfo(const void *sa, socklen_t salen, char *host, socklen_t hostlen,
                  char *serv, socklen_t servlen, int flags)
{
    (void)salen; (void)flags;
    uint16_t fam = *(const uint16_t *)sa;
    if (fam == AF_INET) {
        const struct { uint16_t f, port; uint32_t addr; } *in = sa;
        if (host && hostlen) inet_ntop(AF_INET, &in->addr, host, hostlen);
        if (serv && servlen) snprintf(serv, servlen, "%u", ntohs(in->port));
        return 0;
    }
    if (fam == L_AF_INET6) {
        const uint8_t *b = sa;
        if (host && hostlen) inet_ntop(AF_INET6, b + 8, host, hostlen);
        if (serv && servlen) snprintf(serv, servlen, "%u", ntohs(*(const uint16_t *)(b + 2)));
        return 0;
    }
    return -6;
}

int b_inet_pton(int af, const char *src, void *dst) { return inet_pton(fam_to_bsd(af), src, dst); }
const char *b_inet_ntop(int af, const void *src, char *dst, socklen_t n) { return inet_ntop(fam_to_bsd(af), src, dst, n); }
void *b_gethostbyname(const char *name) { return gethostbyname(name); }
unsigned b_if_nametoindex(const char *n) { (void)n; return 0; }
