/*
 * Created 190923 lynnl
 */

#include "utils.h"
#include "macho.h"

#include <sys/errno.h>
#include <uuid/uuid.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>

typedef struct {
    void *data;
    size_t size;
} buffer_t;

static void *macho_read(buffer_t *buf, void *addr, size_t size)
{
    kassert_nonnull(buf);
    kassert_nonnull(addr);

    if (((uint8_t *) addr - (uint8_t *) buf->data) + size <= + buf->size)
        return addr;

    panicf("macho_read() fail  %zu vs %zu",
            ((uint8_t *) addr - (uint8_t *) buf->data) + size,
            buf->size);
}

//static
void *macho_offset_read(
        buffer_t *buf,
        void *addr,
        off_t off,
        size_t size)
{
    return macho_read(buf, ((uint8_t *) addr) + off, size);
}

static inline uint32_t swap32(uint32_t i)
{
    return OSSwapInt32(i);
}

static inline uint32_t nswap32(uint32_t i)
{
    return i;
}

static const char *mh_magic[] = {
    "magic", "cigam",
};

/**
 * Find LC_UUID load command in a Mach-O executable
 * @return      0 if success, errno otherwise
 */
static int find_LC_UUID0(buffer_t *buf, bool swap, uuid_string_t uuid)
{
    uint32_t (*s32)(uint32_t) = swap ? swap32 : nswap32;
    struct mach_header *h;
    struct load_command *cmd;
    struct uuid_command *ucmd;
    off_t off;
    uint32_t i;

    kassert_nonnull(buf);
    kassert_nonnull(uuid);

    h = macho_read(buf, buf->data, sizeof(*h));

    if (h->magic == MH_MAGIC_64 || h->magic == MH_CIGAM_64) {
        LOG_DBG("64-bit Mach-O %s", mh_magic[h->magic == MH_CIGAM_64]);
        off = sizeof(struct mach_header_64);
    } else if (h->magic == MH_MAGIC || h->magic == MH_CIGAM) {
        LOG_DBG("32-bit Mach-O %s", mh_magic[h->magic == MH_CIGAM]);
        off = sizeof(struct mach_header);
    } else {
        return EBADMACHO;
    }

    for (i = 0; i < s32(h->ncmds); i++) {
        cmd = macho_read(buf, buf->data + off, sizeof(*cmd));

        if (s32(cmd->cmd) != LC_UUID) {
            off += s32(cmd->cmdsize);
            continue;
        }

        kassert_eq(s32(cmd->cmdsize), sizeof(*ucmd));
        ucmd = macho_read(buf, cmd, sizeof(*ucmd));
        uuid_unparse(ucmd->uuid, uuid);
        LOG("Load command index %u\n"
            "       cmd: %u LC_UUID\n"
            "   cmdsize: %u\n"
            "      uuid: %s\n",
            i, s32(cmd->cmd), s32(cmd->cmdsize), uuid);

        return 0;
    }

    return ENOENT;
}

errno_t find_LC_UUID(
        vm_address_t addr,
        vm_size_t size,
        uint32_t flags,
        uuid_string_t uuid)
{
    errno_t e = 0;
    buffer_t buf;
    uint32_t *m;
    bool swap;

    kassert(addr || !size);
    kassert_nonnull(uuid);
    if (size <= sizeof(struct mach_header)) {
        e = EBADMACHO;
        goto out_exit;
    }

    buf.data = (void *) addr;
    buf.size = size;

    m = macho_read(&buf, buf.data, sizeof(*m));
    switch (*m) {
    case MH_MAGIC:
    case MH_CIGAM:
    case MH_MAGIC_64:
    case MH_CIGAM_64: {
        swap = (*m == MH_CIGAM || *m == MH_CIGAM_64);
        e = find_LC_UUID0(&buf, swap, uuid);
        if (e < 0) LOG_ERR("find_LC_UUID0() fail  errno: %d", e);
        break;
    }
    case FAT_MAGIC:
    case FAT_CIGAM: {
        struct fat_header *fh = macho_read(&buf, buf.data, sizeof(*fh));
        uint32_t nfat = OSSwapBigToHostInt32(fh->nfat_arch);
        LOG_DBG("%u arch detected", nfat);
        struct fat_arch *fas = macho_read(&buf, fh + sizeof(*fh), sizeof(*fas));

        uint32_t i;
        struct fat_arch *fa;
        buffer_t sub;
        bool swap;
        for (i = 0; i < nfat; i++) {
            fa = macho_read(&buf, fas + i, sizeof(*fa));
            sub.size = OSSwapBigToHostInt32(fa->size);
            sub.data = macho_read(&buf, fh + OSSwapBigToHostInt32(fa->offset), sub.size);

            m = macho_read(&sub, sub.data, sizeof(*m));
            swap = (*m == MH_CIGAM || *m == MH_CIGAM_64);
            LOG_DBG("Arch #%d (%d-%d) magic: %s",
                    i, fa->cputype, fa->cpusubtype, mh_magic[swap]);

            e = find_LC_UUID0(&sub, swap, uuid);
            if (e != 0) LOG_ERR("find_LC_UUID() fail  i: %u errno: %d", i, e);
        }

        break;
    }
/*
    case FAT_MAGIC_64:
    case FAT_CIGAM_64: {

    }
*/
    default:
        LOG_ERR("Bad magic: %#x", *m);
        e = EBADMACHO;
        break;
    }

out_exit:
    if (e && (flags & MACHO_SET_UUID_FAIL)) {
        (void) snprintf(uuid, sizeof(__typeof__(uuid)), "00000000-0000-0000-0000-000000000000");
    }
    return e;
}

