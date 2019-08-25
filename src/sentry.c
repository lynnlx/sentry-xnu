/*
 * Created 190819 lynnl
 */

#include <stdint.h>

#include <sys/errno.h>
#include <libkern/libkern.h>
#include <uuid/uuid.h>
#include <kern/locks.h>
#include <netinet/in.h>

#include "sentry.h"
#include "utils.h"

#define UUID_BUFSZ              sizeof(uuid_string_t)
/* UUID string buffer size without hyphens */
#define UUID_BUFSZ_COMPACT      (UUID_BUFSZ - 4)

#define SENTRY_DISABLED         0x80u

typedef struct {
    struct in_addr ip;
    uint16_t port;

    char pubkey[UUID_BUFSZ_COMPACT];
    uint64_t projid;
    uint8_t sample_rate;    /* Range: [0, 100] */

    uuid_t last_event_id;

    lck_grp_t *lck_grp;
    lck_rw_t *lck_rw;

    socket_t so;

#if 0
    thread_t thread;
    volatile uint32_t cond_keepalive;
#endif
} sentry_t;

static void sentry_handle_debug(const sentry_t *handle)
{
    uuid_string_t u;

    kassert_nonnull(handle);

    uuid_unparse_lower(handle->last_event_id, u);

    LOG_DBG("Sentry handle %p: "
            "ip: %#010x port: %u pubkey: %s "
            "projid: %llu sample_rate: %u "
            "last_event_id: %s "
            "lck_grp: %p lck_rw: %p "
            "socket: %p",
        handle,
        ntohl(handle->ip.s_addr), handle->port,
        handle->pubkey, handle->projid,
        handle->sample_rate,
        u, handle->lck_grp, handle->lck_rw,
        handle->so);
}

#define HTTP_PORT       80

#define IPV4_BUFSZ      16

static bool parse_ip(sentry_t *handle, const char *host, size_t n)
{
    char buf[IPV4_BUFSZ];

    kassert_nonnull(handle);
    kassert_nonnull(host);

    if (n < 7 || n > 15) return false;
    (void) strlcpy(buf, host, n + 1);

    return inet_aton(buf, &handle->ip);
}

static bool parse_u16(const char *str, size_t n, uint16_t *out)
{
    char buf[6];
    char *p = NULL;
    u_long ul;

    kassert_nonnull(str);
    kassert_nonnull(out);

    if (n == 0 || n >= sizeof(buf)) return false;
    (void) strlcpy(buf, str, n + 1);
    ul = strtoul(buf, &p, 10);

    kassert_nonnull(p);
    if (*p != '\0') return false;

    if ((ul & ~0xffffUL) != 0) return false;

    *out = (uint16_t) ul;
    return true;
}

static bool parse_u64(const char *str, size_t n, uint64_t *out)
{
    char buf[21];
    char *p = NULL;
    uint64_t u64;

    ASSURE_TYPE_ALIAS(u_quad_t, uint64_t);
    kassert_nonnull(str);
    kassert_nonnull(out);

    if (n == 0 || n >= sizeof(buf)) return false;
    (void) strlcpy(buf, str, n + 1);
    u64 = strtouq(buf, &p, 10);

    kassert_nonnull(p);
    if (*p != '\0') return false;

    *out = u64;
    return true;
}

/**
 * DSN(Client key) format:
 *  SCHEME://PUBKEY@HOST[:PORT]/PROJECT_ID
 */
static bool parse_dsn(sentry_t *handle, const char *dsn)
{
    char *p1, *p2;

    kassert_nonnull(handle);
    kassert_nonnull(dsn);

    /* Currently only HTTP scheme is supported */
    if (!striprefix(dsn, "http://")) return false;
    dsn += STRLEN("http://");   /* PUBKEY@HOST[:PORT]/PROJECT_ID */

    p1 = strchr(dsn, '@');
    if (p1 == NULL || p1 - dsn != UUID_BUFSZ_COMPACT - 1) return false;

    (void) strlcpy(handle->pubkey, dsn, UUID_BUFSZ_COMPACT);
    dsn = p1 + 1;               /* HOST[:PORT]/PROJECT_ID */

    p1 = strchr(dsn, ':');
    p2 = strchr(p1 ? p1 + 1 : dsn, '/');
    if (p2 == NULL) return false;

    if (p1 != NULL) {
        if (!parse_ip(handle, dsn, p1 - dsn)) return false;
        if (!parse_u16(p1 + 1, p2 - p1 - 1, &handle->port)) return false;
    } else {
        if (!parse_ip(handle, dsn, p2 - dsn)) return false;
        handle->port = HTTP_PORT;
    }

    dsn = p2 + 1;               /* PROJECT_ID */
    if (!parse_u64(dsn, strlen(dsn), &handle->projid)) return false;
    if (handle->projid == UINT64_MAX) return false;

    return true;
}

/**
 * Create a Sentry handle
 *
 * DSN(Client key) format:
 *  SCHEME://PUBKEY@HOST[:PORT]/PROJECT_ID
 * Currently only HTTP scheme is supported
 *
 * @param handlep       [OUT] pointer to the Sentry handle
 * @param dsn           The client key
 * @param sample_rate   Sample rate [0, 100]
 * @return              0 if success, errno otherwise
 */
int sentry_new(void **handlep, const char *dsn, uint32_t sample_rate)
{
    int e = 0;
    sentry_t h;

    if (handlep == NULL || dsn == NULL || sample_rate > 100) {
        e = EINVAL;
        goto out_exit;
    }

    *handlep = NULL;
    bzero(&h, sizeof(h));

    if (!parse_dsn(&h, dsn)) {
        e = EDOM;
        goto out_exit;
    }

    h.sample_rate = sample_rate;

    /* lck_grp_name is a dummy placeholder */
    h.lck_grp = lck_grp_alloc_init("", LCK_GRP_ATTR_NULL);
    if (h.lck_grp == NULL) {
        e = ENOMEM;
        goto out_exit;
    }

    h.lck_rw = lck_rw_alloc_init(h.lck_grp, LCK_ATTR_NULL);
    if (h.lck_rw == NULL) {
        e = ENOMEM;
        lck_grp_free(h.lck_grp);
        goto out_exit;
    }

    e = sock_socket(PF_INET, SOCK_STREAM, IPPROTO_IP, NULL, NULL, &h.so);
    if (e != 0) {
        lck_rw_free(h.lck_rw, h.lck_grp);
        lck_grp_free(h.lck_grp);
        goto out_exit;
    }

    *handlep = util_malloc(sizeof(h), M_NOWAIT);
    if (*handlep == NULL) {
        e = ENOMEM;
        util_sock_destroy(h.so);
        lck_rw_free(h.lck_rw, h.lck_grp);
        lck_grp_free(h.lck_grp);
        goto out_exit;
    }

    (void) memcpy(*handlep, &h, sizeof(h));

    sentry_handle_debug(*handlep);

out_exit:
    return e;
}

void sentry_destroy(void *handle)
{
    sentry_t *h = (sentry_t *) handle;
    if (h != NULL) {
        util_sock_destroy(h->so);

        lck_rw_free(h->lck_rw, h->lck_grp);
        lck_grp_free(h->lck_grp);

        util_mfree(h);
    }
}
