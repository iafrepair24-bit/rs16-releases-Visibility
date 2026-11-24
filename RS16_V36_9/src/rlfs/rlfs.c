/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "rlfs.h"
#include "rlfs_util.h"


// some constants used throughout the code
#define RLFS_BLOCK_NULL ((rlfs_block_t)-1)
#define RLFS_BLOCK_INLINE ((rlfs_block_t)-2)

enum {
    RLFS_OK_RELOCATED = 1,
    RLFS_OK_DROPPED   = 2,
    RLFS_OK_ORPHANED  = 3,
};

enum {
    RLFS_CMP_EQ = 0,
    RLFS_CMP_LT = 1,
    RLFS_CMP_GT = 2,
};


/// Caching block device operations ///

static inline void rlfs_cache_drop(rlfs_t *lfs, rlfs_cache_t *rcache) {
    // do not zero, cheaper if cache is readonly or only going to be
    // written with identical data (during relocates)
    (void)lfs;
    rcache->block = RLFS_BLOCK_NULL;
}

static inline void rlfs_cache_zero(rlfs_t *lfs, rlfs_cache_t *pcache) {
    // zero to avoid information leak
    memset(pcache->buffer, 0xff, lfs->cfg->cache_size);
    pcache->block = RLFS_BLOCK_NULL;
}

static int rlfs_bd_read(rlfs_t *lfs,
        const rlfs_cache_t *pcache, rlfs_cache_t *rcache, rlfs_size_t hint,
        rlfs_block_t block, rlfs_off_t off,
        void *buffer, rlfs_size_t size) {
    uint8_t *data = buffer;
    if (off+size > lfs->cfg->block_size
            || (lfs->block_count && block >= lfs->block_count)) {
        return RLFS_ERR_CORRUPT;
    }

    while (size > 0) {
        rlfs_size_t diff = size;

        if (pcache && block == pcache->block &&
                off < pcache->off + pcache->size) {
            if (off >= pcache->off) {
                // is already in pcache?
                diff = rlfs_min(diff, pcache->size - (off-pcache->off));
                memcpy(data, &pcache->buffer[off-pcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // pcache takes priority
            diff = rlfs_min(diff, pcache->off-off);
        }

        if (block == rcache->block &&
                off < rcache->off + rcache->size) {
            if (off >= rcache->off) {
                // is already in rcache?
                diff = rlfs_min(diff, rcache->size - (off-rcache->off));
                memcpy(data, &rcache->buffer[off-rcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // rcache takes priority
            diff = rlfs_min(diff, rcache->off-off);
        }

        if (size >= hint && off % lfs->cfg->read_size == 0 &&
                size >= lfs->cfg->read_size) {
            // bypass cache?
            diff = rlfs_aligndown(diff, lfs->cfg->read_size);
            int err = lfs->cfg->read(lfs->cfg, block, off, data, diff);
            RLFS_ASSERT(err <= 0);
            if (err) {
                return err;
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // load to cache, first condition can no longer fail
        RLFS_ASSERT(!lfs->block_count || block < lfs->block_count);
        rcache->block = block;
        rcache->off = rlfs_aligndown(off, lfs->cfg->read_size);
        rcache->size = rlfs_min(
                rlfs_min(
                    rlfs_alignup(off+hint, lfs->cfg->read_size),
                    lfs->cfg->block_size)
                - rcache->off,
                lfs->cfg->cache_size);
        int err = lfs->cfg->read(lfs->cfg, rcache->block,
                rcache->off, rcache->buffer, rcache->size);
        RLFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int rlfs_bd_cmp(rlfs_t *lfs,
        const rlfs_cache_t *pcache, rlfs_cache_t *rcache, rlfs_size_t hint,
        rlfs_block_t block, rlfs_off_t off,
        const void *buffer, rlfs_size_t size) {
    const uint8_t *data = buffer;
    rlfs_size_t diff = 0;

    for (rlfs_off_t i = 0; i < size; i += diff) {
        uint8_t dat[8];

        diff = rlfs_min(size-i, sizeof(dat));
        int err = rlfs_bd_read(lfs,
                pcache, rcache, hint-i,
                block, off+i, &dat, diff);
        if (err) {
            return err;
        }

        int res = memcmp(dat, data + i, diff);
        if (res) {
            return res < 0 ? RLFS_CMP_LT : RLFS_CMP_GT;
        }
    }

    return RLFS_CMP_EQ;
}

static int rlfs_bd_crc(rlfs_t *lfs,
        const rlfs_cache_t *pcache, rlfs_cache_t *rcache, rlfs_size_t hint,
        rlfs_block_t block, rlfs_off_t off, rlfs_size_t size, uint32_t *crc) {
    rlfs_size_t diff = 0;

    for (rlfs_off_t i = 0; i < size; i += diff) {
        uint8_t dat[8];
        diff = rlfs_min(size-i, sizeof(dat));
        int err = rlfs_bd_read(lfs,
                pcache, rcache, hint-i,
                block, off+i, &dat, diff);
        if (err) {
            return err;
        }

        *crc = rlfs_crc(*crc, &dat, diff);
    }

    return 0;
}

#ifndef RLFS_READONLY
static int rlfs_bd_flush(rlfs_t *lfs,
        rlfs_cache_t *pcache, rlfs_cache_t *rcache, bool validate) {
    if (pcache->block != RLFS_BLOCK_NULL && pcache->block != RLFS_BLOCK_INLINE) {
        RLFS_ASSERT(pcache->block < lfs->block_count);
        rlfs_size_t diff = rlfs_alignup(pcache->size, lfs->cfg->prog_size);
        int err = lfs->cfg->prog(lfs->cfg, pcache->block,
                pcache->off, pcache->buffer, diff);
        RLFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }

        if (validate) {
            // check data on disk
            rlfs_cache_drop(lfs, rcache);
            int res = rlfs_bd_cmp(lfs,
                    NULL, rcache, diff,
                    pcache->block, pcache->off, pcache->buffer, diff);
            if (res < 0) {
                return res;
            }

            if (res != RLFS_CMP_EQ) {
                return RLFS_ERR_CORRUPT;
            }
        }

        rlfs_cache_zero(lfs, pcache);
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_bd_sync(rlfs_t *lfs,
        rlfs_cache_t *pcache, rlfs_cache_t *rcache, bool validate) {
    rlfs_cache_drop(lfs, rcache);

    int err = rlfs_bd_flush(lfs, pcache, rcache, validate);
    if (err) {
        return err;
    }

    err = lfs->cfg->sync(lfs->cfg);
    RLFS_ASSERT(err <= 0);
    return err;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_bd_prog(rlfs_t *lfs,
        rlfs_cache_t *pcache, rlfs_cache_t *rcache, bool validate,
        rlfs_block_t block, rlfs_off_t off,
        const void *buffer, rlfs_size_t size) {
    const uint8_t *data = buffer;
    RLFS_ASSERT(block == RLFS_BLOCK_INLINE || block < lfs->block_count);
    RLFS_ASSERT(off + size <= lfs->cfg->block_size);

    while (size > 0) {
        if (block == pcache->block &&
                off >= pcache->off &&
                off < pcache->off + lfs->cfg->cache_size) {
            // already fits in pcache?
            rlfs_size_t diff = rlfs_min(size,
                    lfs->cfg->cache_size - (off-pcache->off));
            memcpy(&pcache->buffer[off-pcache->off], data, diff);

            data += diff;
            off += diff;
            size -= diff;

            pcache->size = rlfs_max(pcache->size, off - pcache->off);
            if (pcache->size == lfs->cfg->cache_size) {
                // eagerly flush out pcache if we fill up
                int err = rlfs_bd_flush(lfs, pcache, rcache, validate);
                if (err) {
                    return err;
                }
            }

            continue;
        }

        // pcache must have been flushed, either by programming and
        // entire block or manually flushing the pcache
        RLFS_ASSERT(pcache->block == RLFS_BLOCK_NULL);

        // prepare pcache, first condition can no longer fail
        pcache->block = block;
        pcache->off = rlfs_aligndown(off, lfs->cfg->prog_size);
        pcache->size = 0;
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_bd_erase(rlfs_t *lfs, rlfs_block_t block) {
    RLFS_ASSERT(block < lfs->block_count);
    int err = lfs->cfg->erase(lfs->cfg, block);
    RLFS_ASSERT(err <= 0);
    return err;
}
#endif


/// Small type-level utilities ///

// some operations on paths
static inline rlfs_size_t rlfs_path_namelen(const char *path) {
    return strcspn(path, "/");
}

static inline bool rlfs_path_islast(const char *path) {
    rlfs_size_t namelen = rlfs_path_namelen(path);
    return path[namelen + strspn(path + namelen, "/")] == '\0';
}

static inline bool rlfs_path_isdir(const char *path) {
    return path[rlfs_path_namelen(path)] != '\0';
}

// operations on block pairs
static inline void rlfs_pair_swap(rlfs_block_t pair[2]) {
    rlfs_block_t t = pair[0];
    pair[0] = pair[1];
    pair[1] = t;
}

static inline bool rlfs_pair_isnull(const rlfs_block_t pair[2]) {
    return pair[0] == RLFS_BLOCK_NULL || pair[1] == RLFS_BLOCK_NULL;
}

static inline int rlfs_pair_cmp(
        const rlfs_block_t paira[2],
        const rlfs_block_t pairb[2]) {
    return !(paira[0] == pairb[0] || paira[1] == pairb[1] ||
             paira[0] == pairb[1] || paira[1] == pairb[0]);
}

static inline bool rlfs_pair_issync(
        const rlfs_block_t paira[2],
        const rlfs_block_t pairb[2]) {
    return (paira[0] == pairb[0] && paira[1] == pairb[1]) ||
           (paira[0] == pairb[1] && paira[1] == pairb[0]);
}

static inline void rlfs_pair_fromle32(rlfs_block_t pair[2]) {
    pair[0] = rlfs_fromle32(pair[0]);
    pair[1] = rlfs_fromle32(pair[1]);
}

#ifndef RLFS_READONLY
static inline void rlfs_pair_tole32(rlfs_block_t pair[2]) {
    pair[0] = rlfs_tole32(pair[0]);
    pair[1] = rlfs_tole32(pair[1]);
}
#endif

// operations on 32-bit entry tags
typedef uint32_t rlfs_tag_t;
typedef int32_t rlfs_stag_t;

#define RLFS_MKTAG(type, id, size) \
    (((rlfs_tag_t)(type) << 20) | ((rlfs_tag_t)(id) << 10) | (rlfs_tag_t)(size))

#define RLFS_MKTAG_IF(cond, type, id, size) \
    ((cond) ? RLFS_MKTAG(type, id, size) : RLFS_MKTAG(RLFS_FROM_NOOP, 0, 0))

#define RLFS_MKTAG_IF_ELSE(cond, type1, id1, size1, type2, id2, size2) \
    ((cond) ? RLFS_MKTAG(type1, id1, size1) : RLFS_MKTAG(type2, id2, size2))

static inline bool rlfs_tag_isvalid(rlfs_tag_t tag) {
    return !(tag & 0x80000000);
}

static inline bool rlfs_tag_isdelete(rlfs_tag_t tag) {
    return ((int32_t)(tag << 22) >> 22) == -1;
}

static inline uint16_t rlfs_tag_type1(rlfs_tag_t tag) {
    return (tag & 0x70000000) >> 20;
}

static inline uint16_t rlfs_tag_type2(rlfs_tag_t tag) {
    return (tag & 0x78000000) >> 20;
}

static inline uint16_t rlfs_tag_type3(rlfs_tag_t tag) {
    return (tag & 0x7ff00000) >> 20;
}

static inline uint8_t rlfs_tag_chunk(rlfs_tag_t tag) {
    return (tag & 0x0ff00000) >> 20;
}

static inline int8_t rlfs_tag_splice(rlfs_tag_t tag) {
    return (int8_t)rlfs_tag_chunk(tag);
}

static inline uint16_t rlfs_tag_id(rlfs_tag_t tag) {
    return (tag & 0x000ffc00) >> 10;
}

static inline rlfs_size_t rlfs_tag_size(rlfs_tag_t tag) {
    return tag & 0x000003ff;
}

static inline rlfs_size_t rlfs_tag_dsize(rlfs_tag_t tag) {
    return sizeof(tag) + rlfs_tag_size(tag + rlfs_tag_isdelete(tag));
}

// operations on attributes in attribute lists
struct rlfs_mattr {
    rlfs_tag_t tag;
    const void *buffer;
};

struct rlfs_diskoff {
    rlfs_block_t block;
    rlfs_off_t off;
};

#define RLFS_MKATTRS(...) \
    (struct rlfs_mattr[]){__VA_ARGS__}, \
    sizeof((struct rlfs_mattr[]){__VA_ARGS__}) / sizeof(struct rlfs_mattr)

// operations on global state
static inline void rlfs_gstate_xor(rlfs_gstate_t *a, const rlfs_gstate_t *b) {
    a->tag ^= b->tag;
    a->pair[0] ^= b->pair[0];
    a->pair[1] ^= b->pair[1];
}

static inline bool rlfs_gstate_iszero(const rlfs_gstate_t *a) {
    return a->tag == 0
            && a->pair[0] == 0
            && a->pair[1] == 0;
}

#ifndef RLFS_READONLY
static inline bool rlfs_gstate_hasorphans(const rlfs_gstate_t *a) {
    return rlfs_tag_size(a->tag);
}

static inline uint8_t rlfs_gstate_getorphans(const rlfs_gstate_t *a) {
    return rlfs_tag_size(a->tag) & 0x1ff;
}

static inline bool rlfs_gstate_hasmove(const rlfs_gstate_t *a) {
    return rlfs_tag_type1(a->tag);
}
#endif

static inline bool rlfs_gstate_needssuperblock(const rlfs_gstate_t *a) {
    return rlfs_tag_size(a->tag) >> 9;
}

static inline bool rlfs_gstate_hasmovehere(const rlfs_gstate_t *a,
        const rlfs_block_t *pair) {
    return rlfs_tag_type1(a->tag) && rlfs_pair_cmp(a->pair, pair) == 0;
}

static inline void rlfs_gstate_fromle32(rlfs_gstate_t *a) {
    a->tag     = rlfs_fromle32(a->tag);
    a->pair[0] = rlfs_fromle32(a->pair[0]);
    a->pair[1] = rlfs_fromle32(a->pair[1]);
}

#ifndef RLFS_READONLY
static inline void rlfs_gstate_tole32(rlfs_gstate_t *a) {
    a->tag     = rlfs_tole32(a->tag);
    a->pair[0] = rlfs_tole32(a->pair[0]);
    a->pair[1] = rlfs_tole32(a->pair[1]);
}
#endif

// operations on forward-CRCs used to track erased state
struct rlfs_fcrc {
    rlfs_size_t size;
    uint32_t crc;
};

static void rlfs_fcrc_fromle32(struct rlfs_fcrc *fcrc) {
    fcrc->size = rlfs_fromle32(fcrc->size);
    fcrc->crc = rlfs_fromle32(fcrc->crc);
}

#ifndef RLFS_READONLY
static void rlfs_fcrc_tole32(struct rlfs_fcrc *fcrc) {
    fcrc->size = rlfs_tole32(fcrc->size);
    fcrc->crc = rlfs_tole32(fcrc->crc);
}
#endif

// other endianness operations
static void rlfs_ctz_fromle32(struct rlfs_ctz *ctz) {
    ctz->head = rlfs_fromle32(ctz->head);
    ctz->size = rlfs_fromle32(ctz->size);
}

#ifndef RLFS_READONLY
static void rlfs_ctz_tole32(struct rlfs_ctz *ctz) {
    ctz->head = rlfs_tole32(ctz->head);
    ctz->size = rlfs_tole32(ctz->size);
}
#endif

static inline void rlfs_superblock_fromle32(rlfs_superblock_t *superblock) {
    superblock->version     = rlfs_fromle32(superblock->version);
    superblock->block_size  = rlfs_fromle32(superblock->block_size);
    superblock->block_count = rlfs_fromle32(superblock->block_count);
    superblock->name_max    = rlfs_fromle32(superblock->name_max);
    superblock->file_max    = rlfs_fromle32(superblock->file_max);
    superblock->attr_max    = rlfs_fromle32(superblock->attr_max);
}

#ifndef RLFS_READONLY
static inline void rlfs_superblock_tole32(rlfs_superblock_t *superblock) {
    superblock->version     = rlfs_tole32(superblock->version);
    superblock->block_size  = rlfs_tole32(superblock->block_size);
    superblock->block_count = rlfs_tole32(superblock->block_count);
    superblock->name_max    = rlfs_tole32(superblock->name_max);
    superblock->file_max    = rlfs_tole32(superblock->file_max);
    superblock->attr_max    = rlfs_tole32(superblock->attr_max);
}
#endif

#ifndef RLFS_NO_ASSERT
static bool rlfs_mlist_isopen(struct rlfs_mlist *head,
        struct rlfs_mlist *node) {
    for (struct rlfs_mlist **p = &head; *p; p = &(*p)->next) {
        if (*p == (struct rlfs_mlist*)node) {
            return true;
        }
    }

    return false;
}
#endif

static void rlfs_mlist_remove(rlfs_t *lfs, struct rlfs_mlist *mlist) {
    for (struct rlfs_mlist **p = &lfs->mlist; *p; p = &(*p)->next) {
        if (*p == mlist) {
            *p = (*p)->next;
            break;
        }
    }
}

static void rlfs_mlist_append(rlfs_t *lfs, struct rlfs_mlist *mlist) {
    mlist->next = lfs->mlist;
    lfs->mlist = mlist;
}

// some other filesystem operations
static uint32_t rlfs_fs_disk_version(rlfs_t *lfs) {
    (void)lfs;
#ifdef RLFS_MULTIVERSION
    if (lfs->cfg->disk_version) {
        return lfs->cfg->disk_version;
    } else
#endif
    {
        return RLFS_DISK_VERSION;
    }
}

static uint16_t rlfs_fs_disk_version_major(rlfs_t *lfs) {
    return 0xffff & (rlfs_fs_disk_version(lfs) >> 16);

}

static uint16_t rlfs_fs_disk_version_minor(rlfs_t *lfs) {
    return 0xffff & (rlfs_fs_disk_version(lfs) >> 0);
}


/// Internal operations predeclared here ///
#ifndef RLFS_READONLY
static int rlfs_dir_commit(rlfs_t *lfs, rlfs_mdir_t *dir,
        const struct rlfs_mattr *attrs, int attrcount);
static int rlfs_dir_compact(rlfs_t *lfs,
        rlfs_mdir_t *dir, const struct rlfs_mattr *attrs, int attrcount,
        rlfs_mdir_t *source, uint16_t begin, uint16_t end);
static rlfs_ssize_t rlfs_file_flushedwrite(rlfs_t *lfs, rlfs_file_t *file,
        const void *buffer, rlfs_size_t size);
static rlfs_ssize_t rlfs_file_write_(rlfs_t *lfs, rlfs_file_t *file,
        const void *buffer, rlfs_size_t size);
static int rlfs_file_sync_(rlfs_t *lfs, rlfs_file_t *file);
static int rlfs_file_outline(rlfs_t *lfs, rlfs_file_t *file);
static int rlfs_file_flush(rlfs_t *lfs, rlfs_file_t *file);

static int rlfs_fs_deorphan(rlfs_t *lfs, bool powerloss);
static int rlfs_fs_preporphans(rlfs_t *lfs, int8_t orphans);
static void rlfs_fs_prepmove(rlfs_t *lfs,
        uint16_t id, const rlfs_block_t pair[2]);
static int rlfs_fs_pred(rlfs_t *lfs, const rlfs_block_t dir[2],
        rlfs_mdir_t *pdir);
static rlfs_stag_t rlfs_fs_parent(rlfs_t *lfs, const rlfs_block_t dir[2],
        rlfs_mdir_t *parent);
static int rlfs_fs_forceconsistency(rlfs_t *lfs);
#endif

static void rlfs_fs_prepsuperblock(rlfs_t *lfs, bool needssuperblock);

#ifdef RLFS_MIGRATE
static int lfs1_traverse(rlfs_t *lfs,
        int (*cb)(void*, rlfs_block_t), void *data);
#endif

static int rlfs_dir_rewind_(rlfs_t *lfs, rlfs_dir_t *dir);

static rlfs_ssize_t rlfs_file_flushedread(rlfs_t *lfs, rlfs_file_t *file,
        void *buffer, rlfs_size_t size);
static rlfs_ssize_t rlfs_file_read_(rlfs_t *lfs, rlfs_file_t *file,
        void *buffer, rlfs_size_t size);
static int rlfs_file_close_(rlfs_t *lfs, rlfs_file_t *file);
static rlfs_soff_t rlfs_file_size_(rlfs_t *lfs, rlfs_file_t *file);

static rlfs_ssize_t rlfs_fs_size_(rlfs_t *lfs);
static int rlfs_fs_traverse_(rlfs_t *lfs,
        int (*cb)(void *data, rlfs_block_t block), void *data,
        bool includeorphans);

static int rlfs_deinit(rlfs_t *lfs);
static int rlfs_unmount_(rlfs_t *lfs);


/// Block allocator ///

// allocations should call this when all allocated blocks are committed to
// the filesystem
//
// after a checkpoint, the block allocator may realloc any untracked blocks
static void rlfs_alloc_ckpoint(rlfs_t *lfs) {
    lfs->lookahead.ckpoint = lfs->block_count;
}

// drop the lookahead buffer, this is done during mounting and failed
// traversals in order to avoid invalid lookahead state
static void rlfs_alloc_drop(rlfs_t *lfs) {
    lfs->lookahead.size = 0;
    lfs->lookahead.next = 0;
    rlfs_alloc_ckpoint(lfs);
}

#ifndef RLFS_READONLY
static int rlfs_alloc_lookahead(void *p, rlfs_block_t block) {
    rlfs_t *lfs = (rlfs_t*)p;
    rlfs_block_t off = ((block - lfs->lookahead.start)
            + lfs->block_count) % lfs->block_count;

    if (off < lfs->lookahead.size) {
        lfs->lookahead.buffer[off / 8] |= 1U << (off % 8);
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_alloc_scan(rlfs_t *lfs) {
    // move lookahead buffer to the first unused block
    //
    // note we limit the lookahead buffer to at most the amount of blocks
    // checkpointed, this prevents the math in rlfs_alloc from underflowing
    lfs->lookahead.start = (lfs->lookahead.start + lfs->lookahead.next) 
            % lfs->block_count;
    lfs->lookahead.next = 0;
    lfs->lookahead.size = rlfs_min(
            8*lfs->cfg->lookahead_size,
            lfs->lookahead.ckpoint);

    // find mask of free blocks from tree
    memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);
    int err = rlfs_fs_traverse_(lfs, rlfs_alloc_lookahead, lfs, true);
    if (err) {
        rlfs_alloc_drop(lfs);
        return err;
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_alloc(rlfs_t *lfs, rlfs_block_t *block) {
    while (true) {
        // scan our lookahead buffer for free blocks
        while (lfs->lookahead.next < lfs->lookahead.size) {
            if (!(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                    & (1U << (lfs->lookahead.next % 8)))) {
                // found a free block
                *block = (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->block_count;

                // eagerly find next free block to maximize how many blocks
                // rlfs_alloc_ckpoint makes available for scanning
                while (true) {
                    lfs->lookahead.next += 1;
                    lfs->lookahead.ckpoint -= 1;

                    if (lfs->lookahead.next >= lfs->lookahead.size
                            || !(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                                & (1U << (lfs->lookahead.next % 8)))) {
                        return 0;
                    }
                }
            }

            lfs->lookahead.next += 1;
            lfs->lookahead.ckpoint -= 1;
        }

        // In order to keep our block allocator from spinning forever when our
        // filesystem is full, we mark points where there are no in-flight
        // allocations with a checkpoint before starting a set of allocations.
        //
        // If we've looked at all blocks since the last checkpoint, we report
        // the filesystem as out of storage.
        //
        if (lfs->lookahead.ckpoint <= 0) {
            RLFS_ERROR("No more free space 0x%"PRIx32,
                    (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->block_count);
            return RLFS_ERR_NOSPC;
        }

        // No blocks in our lookahead buffer, we need to scan the filesystem for
        // unused blocks in the next lookahead window.
        int err = rlfs_alloc_scan(lfs);
        if(err) {
            return err;
        }
    }
}
#endif

/// Metadata pair and directory operations ///
static rlfs_stag_t rlfs_dir_getslice(rlfs_t *lfs, const rlfs_mdir_t *dir,
        rlfs_tag_t gmask, rlfs_tag_t gtag,
        rlfs_off_t goff, void *gbuffer, rlfs_size_t gsize) {
    rlfs_off_t off = dir->off;
    rlfs_tag_t ntag = dir->etag;
    rlfs_stag_t gdiff = 0;

    // synthetic moves
    if (rlfs_gstate_hasmovehere(&lfs->gdisk, dir->pair) &&
            rlfs_tag_id(gmask) != 0) {
        if (rlfs_tag_id(lfs->gdisk.tag) == rlfs_tag_id(gtag)) {
            return RLFS_ERR_NOENT;
        } else if (rlfs_tag_id(lfs->gdisk.tag) < rlfs_tag_id(gtag)) {
            gdiff -= RLFS_MKTAG(0, 1, 0);
        }
    }

    // iterate over dir block backwards (for faster lookups)
    while (off >= sizeof(rlfs_tag_t) + rlfs_tag_dsize(ntag)) {
        off -= rlfs_tag_dsize(ntag);
        rlfs_tag_t tag = ntag;
        int err = rlfs_bd_read(lfs,
                NULL, &lfs->rcache, sizeof(ntag),
                dir->pair[0], off, &ntag, sizeof(ntag));
        RLFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }

        ntag = (rlfs_frombe32(ntag) ^ tag) & 0x7fffffff;

        if (rlfs_tag_id(gmask) != 0 &&
                rlfs_tag_type1(tag) == RLFS_TYPE_SPLICE &&
                rlfs_tag_id(tag) <= rlfs_tag_id(gtag - gdiff)) {
            if (tag == (RLFS_MKTAG(RLFS_TYPE_CREATE, 0, 0) |
                    (RLFS_MKTAG(0, 0x3ff, 0) & (gtag - gdiff)))) {
                // found where we were created
                return RLFS_ERR_NOENT;
            }

            // move around splices
            gdiff += RLFS_MKTAG(0, rlfs_tag_splice(tag), 0);
        }

        if ((gmask & tag) == (gmask & (gtag - gdiff))) {
            if (rlfs_tag_isdelete(tag)) {
                return RLFS_ERR_NOENT;
            }

            rlfs_size_t diff = rlfs_min(rlfs_tag_size(tag), gsize);
            err = rlfs_bd_read(lfs,
                    NULL, &lfs->rcache, diff,
                    dir->pair[0], off+sizeof(tag)+goff, gbuffer, diff);
            RLFS_ASSERT(err <= 0);
            if (err) {
                return err;
            }

            memset((uint8_t*)gbuffer + diff, 0, gsize - diff);

            return tag + gdiff;
        }
    }

    return RLFS_ERR_NOENT;
}

static rlfs_stag_t rlfs_dir_get(rlfs_t *lfs, const rlfs_mdir_t *dir,
        rlfs_tag_t gmask, rlfs_tag_t gtag, void *buffer) {
    return rlfs_dir_getslice(lfs, dir,
            gmask, gtag,
            0, buffer, rlfs_tag_size(gtag));
}

static int rlfs_dir_getread(rlfs_t *lfs, const rlfs_mdir_t *dir,
        const rlfs_cache_t *pcache, rlfs_cache_t *rcache, rlfs_size_t hint,
        rlfs_tag_t gmask, rlfs_tag_t gtag,
        rlfs_off_t off, void *buffer, rlfs_size_t size) {
    uint8_t *data = buffer;
    if (off+size > lfs->cfg->block_size) {
        return RLFS_ERR_CORRUPT;
    }

    while (size > 0) {
        rlfs_size_t diff = size;

        if (pcache && pcache->block == RLFS_BLOCK_INLINE &&
                off < pcache->off + pcache->size) {
            if (off >= pcache->off) {
                // is already in pcache?
                diff = rlfs_min(diff, pcache->size - (off-pcache->off));
                memcpy(data, &pcache->buffer[off-pcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // pcache takes priority
            diff = rlfs_min(diff, pcache->off-off);
        }

        if (rcache->block == RLFS_BLOCK_INLINE &&
                off < rcache->off + rcache->size) {
            if (off >= rcache->off) {
                // is already in rcache?
                diff = rlfs_min(diff, rcache->size - (off-rcache->off));
                memcpy(data, &rcache->buffer[off-rcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }
        }

        // load to cache, first condition can no longer fail
        rcache->block = RLFS_BLOCK_INLINE;
        rcache->off = rlfs_aligndown(off, lfs->cfg->read_size);
        rcache->size = rlfs_min(rlfs_alignup(off+hint, lfs->cfg->read_size),
                lfs->cfg->cache_size);
        int err = rlfs_dir_getslice(lfs, dir, gmask, gtag,
                rcache->off, rcache->buffer, rcache->size);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

#ifndef RLFS_READONLY
static int rlfs_dir_traverse_filter(void *p,
        rlfs_tag_t tag, const void *buffer) {
    rlfs_tag_t *filtertag = p;
    (void)buffer;

    // which mask depends on unique bit in tag structure
    uint32_t mask = (tag & RLFS_MKTAG(0x100, 0, 0))
            ? RLFS_MKTAG(0x7ff, 0x3ff, 0)
            : RLFS_MKTAG(0x700, 0x3ff, 0);

    // check for redundancy
    if ((mask & tag) == (mask & *filtertag) ||
            rlfs_tag_isdelete(*filtertag) ||
            (RLFS_MKTAG(0x7ff, 0x3ff, 0) & tag) == (
                RLFS_MKTAG(RLFS_TYPE_DELETE, 0, 0) |
                    (RLFS_MKTAG(0, 0x3ff, 0) & *filtertag))) {
        *filtertag = RLFS_MKTAG(RLFS_FROM_NOOP, 0, 0);
        return true;
    }

    // check if we need to adjust for created/deleted tags
    if (rlfs_tag_type1(tag) == RLFS_TYPE_SPLICE &&
            rlfs_tag_id(tag) <= rlfs_tag_id(*filtertag)) {
        *filtertag += RLFS_MKTAG(0, rlfs_tag_splice(tag), 0);
    }

    return false;
}
#endif

#ifndef RLFS_READONLY
// maximum recursive depth of rlfs_dir_traverse, the deepest call:
//
// traverse with commit
// '-> traverse with move
//     '-> traverse with filter
//
#define RLFS_DIR_TRAVERSE_DEPTH 3

struct rlfs_dir_traverse {
    const rlfs_mdir_t *dir;
    rlfs_off_t off;
    rlfs_tag_t ptag;
    const struct rlfs_mattr *attrs;
    int attrcount;

    rlfs_tag_t tmask;
    rlfs_tag_t ttag;
    uint16_t begin;
    uint16_t end;
    int16_t diff;

    int (*cb)(void *data, rlfs_tag_t tag, const void *buffer);
    void *data;

    rlfs_tag_t tag;
    const void *buffer;
    struct rlfs_diskoff disk;
};

static int rlfs_dir_traverse(rlfs_t *lfs,
        const rlfs_mdir_t *dir, rlfs_off_t off, rlfs_tag_t ptag,
        const struct rlfs_mattr *attrs, int attrcount,
        rlfs_tag_t tmask, rlfs_tag_t ttag,
        uint16_t begin, uint16_t end, int16_t diff,
        int (*cb)(void *data, rlfs_tag_t tag, const void *buffer), void *data) {
    // This function in inherently recursive, but bounded. To allow tool-based
    // analysis without unnecessary code-cost we use an explicit stack
    struct rlfs_dir_traverse stack[RLFS_DIR_TRAVERSE_DEPTH-1];
    unsigned sp = 0;
    int res;

    // iterate over directory and attrs
    rlfs_tag_t tag;
    const void *buffer;
    struct rlfs_diskoff disk = {0};
    while (true) {
        {
            if (off+rlfs_tag_dsize(ptag) < dir->off) {
                off += rlfs_tag_dsize(ptag);
                int err = rlfs_bd_read(lfs,
                        NULL, &lfs->rcache, sizeof(tag),
                        dir->pair[0], off, &tag, sizeof(tag));
                if (err) {
                    return err;
                }

                tag = (rlfs_frombe32(tag) ^ ptag) | 0x80000000;
                disk.block = dir->pair[0];
                disk.off = off+sizeof(rlfs_tag_t);
                buffer = &disk;
                ptag = tag;
            } else if (attrcount > 0) {
                tag = attrs[0].tag;
                buffer = attrs[0].buffer;
                attrs += 1;
                attrcount -= 1;
            } else {
                // finished traversal, pop from stack?
                res = 0;
                break;
            }

            // do we need to filter?
            rlfs_tag_t mask = RLFS_MKTAG(0x7ff, 0, 0);
            if ((mask & tmask & tag) != (mask & tmask & ttag)) {
                continue;
            }

            if (rlfs_tag_id(tmask) != 0) {
                RLFS_ASSERT(sp < RLFS_DIR_TRAVERSE_DEPTH);
                // recurse, scan for duplicates, and update tag based on
                // creates/deletes
                stack[sp] = (struct rlfs_dir_traverse){
                    .dir        = dir,
                    .off        = off,
                    .ptag       = ptag,
                    .attrs      = attrs,
                    .attrcount  = attrcount,
                    .tmask      = tmask,
                    .ttag       = ttag,
                    .begin      = begin,
                    .end        = end,
                    .diff       = diff,
                    .cb         = cb,
                    .data       = data,
                    .tag        = tag,
                    .buffer     = buffer,
                    .disk       = disk,
                };
                sp += 1;

                tmask = 0;
                ttag = 0;
                begin = 0;
                end = 0;
                diff = 0;
                cb = rlfs_dir_traverse_filter;
                data = &stack[sp-1].tag;
                continue;
            }
        }

popped:
        // in filter range?
        if (rlfs_tag_id(tmask) != 0 &&
                !(rlfs_tag_id(tag) >= begin && rlfs_tag_id(tag) < end)) {
            continue;
        }

        // handle special cases for mcu-side operations
        if (rlfs_tag_type3(tag) == RLFS_FROM_NOOP) {
            // do nothing
        } else if (rlfs_tag_type3(tag) == RLFS_FROM_MOVE) {
            // Without this condition, rlfs_dir_traverse can exhibit an
            // extremely expensive O(n^3) of nested loops when renaming.
            // This happens because rlfs_dir_traverse tries to filter tags by
            // the tags in the source directory, triggering a second
            // rlfs_dir_traverse with its own filter operation.
            //
            // traverse with commit
            // '-> traverse with filter
            //     '-> traverse with move
            //         '-> traverse with filter
            //
            // However we don't actually care about filtering the second set of
            // tags, since duplicate tags have no effect when filtering.
            //
            // This check skips this unnecessary recursive filtering explicitly,
            // reducing this runtime from O(n^3) to O(n^2).
            if (cb == rlfs_dir_traverse_filter) {
                continue;
            }

            // recurse into move
            stack[sp] = (struct rlfs_dir_traverse){
                .dir        = dir,
                .off        = off,
                .ptag       = ptag,
                .attrs      = attrs,
                .attrcount  = attrcount,
                .tmask      = tmask,
                .ttag       = ttag,
                .begin      = begin,
                .end        = end,
                .diff       = diff,
                .cb         = cb,
                .data       = data,
                .tag        = RLFS_MKTAG(RLFS_FROM_NOOP, 0, 0),
            };
            sp += 1;

            uint16_t fromid = rlfs_tag_size(tag);
            uint16_t toid = rlfs_tag_id(tag);
            dir = buffer;
            off = 0;
            ptag = 0xffffffff;
            attrs = NULL;
            attrcount = 0;
            tmask = RLFS_MKTAG(0x600, 0x3ff, 0);
            ttag = RLFS_MKTAG(RLFS_TYPE_STRUCT, 0, 0);
            begin = fromid;
            end = fromid+1;
            diff = toid-fromid+diff;
        } else if (rlfs_tag_type3(tag) == RLFS_FROM_USERATTRS) {
            for (unsigned i = 0; i < rlfs_tag_size(tag); i++) {
                const struct rlfs_attr *a = buffer;
                res = cb(data, RLFS_MKTAG(RLFS_TYPE_USERATTR + a[i].type,
                        rlfs_tag_id(tag) + diff, a[i].size), a[i].buffer);
                if (res < 0) {
                    return res;
                }

                if (res) {
                    break;
                }
            }
        } else {
            res = cb(data, tag + RLFS_MKTAG(0, diff, 0), buffer);
            if (res < 0) {
                return res;
            }

            if (res) {
                break;
            }
        }
    }

    if (sp > 0) {
        // pop from the stack and return, fortunately all pops share
        // a destination
        dir         = stack[sp-1].dir;
        off         = stack[sp-1].off;
        ptag        = stack[sp-1].ptag;
        attrs       = stack[sp-1].attrs;
        attrcount   = stack[sp-1].attrcount;
        tmask       = stack[sp-1].tmask;
        ttag        = stack[sp-1].ttag;
        begin       = stack[sp-1].begin;
        end         = stack[sp-1].end;
        diff        = stack[sp-1].diff;
        cb          = stack[sp-1].cb;
        data        = stack[sp-1].data;
        tag         = stack[sp-1].tag;
        buffer      = stack[sp-1].buffer;
        disk        = stack[sp-1].disk;
        sp -= 1;
        goto popped;
    } else {
        return res;
    }
}
#endif

static rlfs_stag_t rlfs_dir_fetchmatch(rlfs_t *lfs,
        rlfs_mdir_t *dir, const rlfs_block_t pair[2],
        rlfs_tag_t fmask, rlfs_tag_t ftag, uint16_t *id,
        int (*cb)(void *data, rlfs_tag_t tag, const void *buffer), void *data) {
    // we can find tag very efficiently during a fetch, since we're already
    // scanning the entire directory
    rlfs_stag_t besttag = -1;

    // if either block address is invalid we return RLFS_ERR_CORRUPT here,
    // otherwise later writes to the pair could fail
    if (lfs->block_count 
            && (pair[0] >= lfs->block_count || pair[1] >= lfs->block_count)) {
        return RLFS_ERR_CORRUPT;
    }

    // find the block with the most recent revision
    uint32_t revs[2] = {0, 0};
    int r = 0;
    for (int i = 0; i < 2; i++) {
        int err = rlfs_bd_read(lfs,
                NULL, &lfs->rcache, sizeof(revs[i]),
                pair[i], 0, &revs[i], sizeof(revs[i]));
        revs[i] = rlfs_fromle32(revs[i]);
        if (err && err != RLFS_ERR_CORRUPT) {
            return err;
        }

        if (err != RLFS_ERR_CORRUPT &&
                rlfs_scmp(revs[i], revs[(i+1)%2]) > 0) {
            r = i;
        }
    }

    dir->pair[0] = pair[(r+0)%2];
    dir->pair[1] = pair[(r+1)%2];
    dir->rev = revs[(r+0)%2];
    dir->off = 0; // nonzero = found some commits

    // now scan tags to fetch the actual dir and find possible match
    for (int i = 0; i < 2; i++) {
        rlfs_off_t off = 0;
        rlfs_tag_t ptag = 0xffffffff;

        uint16_t tempcount = 0;
        rlfs_block_t temptail[2] = {RLFS_BLOCK_NULL, RLFS_BLOCK_NULL};
        bool tempsplit = false;
        rlfs_stag_t tempbesttag = besttag;

        // assume not erased until proven otherwise
        bool maybeerased = false;
        bool hasfcrc = false;
        struct rlfs_fcrc fcrc;

        dir->rev = rlfs_tole32(dir->rev);
        uint32_t crc = rlfs_crc(0xffffffff, &dir->rev, sizeof(dir->rev));
        dir->rev = rlfs_fromle32(dir->rev);

        while (true) {
            // extract next tag
            rlfs_tag_t tag;
            off += rlfs_tag_dsize(ptag);
            int err = rlfs_bd_read(lfs,
                    NULL, &lfs->rcache, lfs->cfg->block_size,
                    dir->pair[0], off, &tag, sizeof(tag));
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    // can't continue?
                    break;
                }
                return err;
            }

            crc = rlfs_crc(crc, &tag, sizeof(tag));
            tag = rlfs_frombe32(tag) ^ ptag;

            // next commit not yet programmed?
            if (!rlfs_tag_isvalid(tag)) {
                // we only might be erased if the last tag was a crc
                maybeerased = (rlfs_tag_type2(ptag) == RLFS_TYPE_CCRC);
                break;
            // out of range?
            } else if (off + rlfs_tag_dsize(tag) > lfs->cfg->block_size) {
                break;
            }

            ptag = tag;

            if (rlfs_tag_type2(tag) == RLFS_TYPE_CCRC) {
                // check the crc attr
                uint32_t dcrc;
                err = rlfs_bd_read(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], off+sizeof(tag), &dcrc, sizeof(dcrc));
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                dcrc = rlfs_fromle32(dcrc);

                if (crc != dcrc) {
                    break;
                }

                // reset the next bit if we need to
                ptag ^= (rlfs_tag_t)(rlfs_tag_chunk(tag) & 1U) << 31;

                // toss our crc into the filesystem seed for
                // pseudorandom numbers, note we use another crc here
                // as a collection function because it is sufficiently
                // random and convenient
                lfs->seed = rlfs_crc(lfs->seed, &crc, sizeof(crc));

                // update with what's found so far
                besttag = tempbesttag;
                dir->off = off + rlfs_tag_dsize(tag);
                dir->etag = ptag;
                dir->count = tempcount;
                dir->tail[0] = temptail[0];
                dir->tail[1] = temptail[1];
                dir->split = tempsplit;

                // reset crc, hasfcrc
                crc = 0xffffffff;
                continue;
            }

            // crc the entry first, hopefully leaving it in the cache
            err = rlfs_bd_crc(lfs,
                    NULL, &lfs->rcache, lfs->cfg->block_size,
                    dir->pair[0], off+sizeof(tag),
                    rlfs_tag_dsize(tag)-sizeof(tag), &crc);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    break;
                }
                return err;
            }

            // directory modification tags?
            if (rlfs_tag_type1(tag) == RLFS_TYPE_NAME) {
                // increase count of files if necessary
                if (rlfs_tag_id(tag) >= tempcount) {
                    tempcount = rlfs_tag_id(tag) + 1;
                }
            } else if (rlfs_tag_type1(tag) == RLFS_TYPE_SPLICE) {
                tempcount += rlfs_tag_splice(tag);

                if (tag == (RLFS_MKTAG(RLFS_TYPE_DELETE, 0, 0) |
                        (RLFS_MKTAG(0, 0x3ff, 0) & tempbesttag))) {
                    tempbesttag |= 0x80000000;
                } else if (tempbesttag != -1 &&
                        rlfs_tag_id(tag) <= rlfs_tag_id(tempbesttag)) {
                    tempbesttag += RLFS_MKTAG(0, rlfs_tag_splice(tag), 0);
                }
            } else if (rlfs_tag_type1(tag) == RLFS_TYPE_TAIL) {
                tempsplit = (rlfs_tag_chunk(tag) & 1);

                err = rlfs_bd_read(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], off+sizeof(tag), &temptail, 8);
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                rlfs_pair_fromle32(temptail);
            } else if (rlfs_tag_type3(tag) == RLFS_TYPE_FCRC) {
                err = rlfs_bd_read(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], off+sizeof(tag),
                        &fcrc, sizeof(fcrc));
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }

                rlfs_fcrc_fromle32(&fcrc);
                hasfcrc = true;
            }

            // found a match for our fetcher?
            if ((fmask & tag) == (fmask & ftag)) {
                int res = cb(data, tag, &(struct rlfs_diskoff){
                        dir->pair[0], off+sizeof(tag)});
                if (res < 0) {
                    if (res == RLFS_ERR_CORRUPT) {
                        break;
                    }
                    return res;
                }

                if (res == RLFS_CMP_EQ) {
                    // found a match
                    tempbesttag = tag;
                } else if ((RLFS_MKTAG(0x7ff, 0x3ff, 0) & tag) ==
                        (RLFS_MKTAG(0x7ff, 0x3ff, 0) & tempbesttag)) {
                    // found an identical tag, but contents didn't match
                    // this must mean that our besttag has been overwritten
                    tempbesttag = -1;
                } else if (res == RLFS_CMP_GT &&
                        rlfs_tag_id(tag) <= rlfs_tag_id(tempbesttag)) {
                    // found a greater match, keep track to keep things sorted
                    tempbesttag = tag | 0x80000000;
                }
            }
        }

        // found no valid commits?
        if (dir->off == 0) {
            // try the other block?
            rlfs_pair_swap(dir->pair);
            dir->rev = revs[(r+1)%2];
            continue;
        }

        // did we end on a valid commit? we may have an erased block
        dir->erased = false;
        if (maybeerased && dir->off % lfs->cfg->prog_size == 0) {
        #ifdef RLFS_MULTIVERSION
            // note versions < lfs2.1 did not have fcrc tags, if
            // we're < lfs2.1 treat missing fcrc as erased data
            //
            // we don't strictly need to do this, but otherwise writing
            // to lfs2.0 disks becomes very inefficient
            if (rlfs_fs_disk_version(lfs) < 0x00020001) {
                dir->erased = true;

            } else
        #endif
            if (hasfcrc) {
                // check for an fcrc matching the next prog's erased state, if
                // this failed most likely a previous prog was interrupted, we
                // need a new erase
                uint32_t fcrc_ = 0xffffffff;
                int err = rlfs_bd_crc(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], dir->off, fcrc.size, &fcrc_);
                if (err && err != RLFS_ERR_CORRUPT) {
                    return err;
                }

                // found beginning of erased part?
                dir->erased = (fcrc_ == fcrc.crc);
            }
        }

        // synthetic move
        if (rlfs_gstate_hasmovehere(&lfs->gdisk, dir->pair)) {
            if (rlfs_tag_id(lfs->gdisk.tag) == rlfs_tag_id(besttag)) {
                besttag |= 0x80000000;
            } else if (besttag != -1 &&
                    rlfs_tag_id(lfs->gdisk.tag) < rlfs_tag_id(besttag)) {
                besttag -= RLFS_MKTAG(0, 1, 0);
            }
        }

        // found tag? or found best id?
        if (id) {
            *id = rlfs_min(rlfs_tag_id(besttag), dir->count);
        }

        if (rlfs_tag_isvalid(besttag)) {
            return besttag;
        } else if (rlfs_tag_id(besttag) < dir->count) {
            return RLFS_ERR_NOENT;
        } else {
            return 0;
        }
    }

    RLFS_ERROR("Corrupted dir pair at {0x%"PRIx32", 0x%"PRIx32"}",
            dir->pair[0], dir->pair[1]);
    return RLFS_ERR_CORRUPT;
}

static int rlfs_dir_fetch(rlfs_t *lfs,
        rlfs_mdir_t *dir, const rlfs_block_t pair[2]) {
    // note, mask=-1, tag=-1 can never match a tag since this
    // pattern has the invalid bit set
    return (int)rlfs_dir_fetchmatch(lfs, dir, pair,
            (rlfs_tag_t)-1, (rlfs_tag_t)-1, NULL, NULL, NULL);
}

static int rlfs_dir_getgstate(rlfs_t *lfs, const rlfs_mdir_t *dir,
        rlfs_gstate_t *gstate) {
    rlfs_gstate_t temp;
    rlfs_stag_t res = rlfs_dir_get(lfs, dir, RLFS_MKTAG(0x7ff, 0, 0),
            RLFS_MKTAG(RLFS_TYPE_MOVESTATE, 0, sizeof(temp)), &temp);
    if (res < 0 && res != RLFS_ERR_NOENT) {
        return res;
    }

    if (res != RLFS_ERR_NOENT) {
        // xor together to find resulting gstate
        rlfs_gstate_fromle32(&temp);
        rlfs_gstate_xor(gstate, &temp);
    }

    return 0;
}

static int rlfs_dir_getinfo(rlfs_t *lfs, rlfs_mdir_t *dir,
        uint16_t id, struct rlfs_info *info) {
    if (id == 0x3ff) {
        // special case for root
        strcpy(info->name, "/");
        info->type = RLFS_TYPE_DIR;
        return 0;
    }

    rlfs_stag_t tag = rlfs_dir_get(lfs, dir, RLFS_MKTAG(0x780, 0x3ff, 0),
            RLFS_MKTAG(RLFS_TYPE_NAME, id, lfs->name_max+1), info->name);
    if (tag < 0) {
        return (int)tag;
    }

    info->type = rlfs_tag_type3(tag);

    struct rlfs_ctz ctz;
    tag = rlfs_dir_get(lfs, dir, RLFS_MKTAG(0x700, 0x3ff, 0),
            RLFS_MKTAG(RLFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
    if (tag < 0) {
        return (int)tag;
    }
    rlfs_ctz_fromle32(&ctz);

    if (rlfs_tag_type3(tag) == RLFS_TYPE_CTZSTRUCT) {
        info->size = ctz.size;
    } else if (rlfs_tag_type3(tag) == RLFS_TYPE_INLINESTRUCT) {
        info->size = rlfs_tag_size(tag);
    }

    return 0;
}

struct rlfs_dir_find_match {
    rlfs_t *lfs;
    const void *name;
    rlfs_size_t size;
};

static int rlfs_dir_find_match(void *data,
        rlfs_tag_t tag, const void *buffer) {
    struct rlfs_dir_find_match *name = data;
    rlfs_t *lfs = name->lfs;
    const struct rlfs_diskoff *disk = buffer;

    // compare with disk
    rlfs_size_t diff = rlfs_min(name->size, rlfs_tag_size(tag));
    int res = rlfs_bd_cmp(lfs,
            NULL, &lfs->rcache, diff,
            disk->block, disk->off, name->name, diff);
    if (res != RLFS_CMP_EQ) {
        return res;
    }

    // only equal if our size is still the same
    if (name->size != rlfs_tag_size(tag)) {
        return (name->size < rlfs_tag_size(tag)) ? RLFS_CMP_LT : RLFS_CMP_GT;
    }

    // found a match!
    return RLFS_CMP_EQ;
}

// rlfs_dir_find tries to set path and id even if file is not found
//
// returns:
// - 0                  if file is found
// - RLFS_ERR_NOENT      if file or parent is not found
// - RLFS_ERR_NOTDIR     if parent is not a dir
static rlfs_stag_t rlfs_dir_find(rlfs_t *lfs, rlfs_mdir_t *dir,
        const char **path, uint16_t *id) {
    // we reduce path to a single name if we can find it
    const char *name = *path;

    // default to root dir
    rlfs_stag_t tag = RLFS_MKTAG(RLFS_TYPE_DIR, 0x3ff, 0);
    dir->tail[0] = lfs->root[0];
    dir->tail[1] = lfs->root[1];

    // empty paths are not allowed
    if (*name == '\0') {
        return RLFS_ERR_INVAL;
    }

    while (true) {
nextname:
        // skip slashes if we're a directory
        if (rlfs_tag_type3(tag) == RLFS_TYPE_DIR) {
            name += strspn(name, "/");
        }
        rlfs_size_t namelen = strcspn(name, "/");

        // skip '.'
        if (namelen == 1 && memcmp(name, ".", 1) == 0) {
            name += namelen;
            goto nextname;
        }

        // error on unmatched '..', trying to go above root?
        if (namelen == 2 && memcmp(name, "..", 2) == 0) {
            return RLFS_ERR_INVAL;
        }

        // skip if matched by '..' in name
        const char *suffix = name + namelen;
        rlfs_size_t sufflen;
        int depth = 1;
        while (true) {
            suffix += strspn(suffix, "/");
            sufflen = strcspn(suffix, "/");
            if (sufflen == 0) {
                break;
            }

            if (sufflen == 1 && memcmp(suffix, ".", 1) == 0) {
                // noop
            } else if (sufflen == 2 && memcmp(suffix, "..", 2) == 0) {
                depth -= 1;
                if (depth == 0) {
                    name = suffix + sufflen;
                    goto nextname;
                }
            } else {
                depth += 1;
            }

            suffix += sufflen;
        }

        // found path
        if (*name == '\0') {
            return tag;
        }

        // update what we've found so far
        *path = name;

        // only continue if we're a directory
        if (rlfs_tag_type3(tag) != RLFS_TYPE_DIR) {
            return RLFS_ERR_NOTDIR;
        }

        // grab the entry data
        if (rlfs_tag_id(tag) != 0x3ff) {
            rlfs_stag_t res = rlfs_dir_get(lfs, dir, RLFS_MKTAG(0x700, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_STRUCT, rlfs_tag_id(tag), 8), dir->tail);
            if (res < 0) {
                return res;
            }
            rlfs_pair_fromle32(dir->tail);
        }

        // find entry matching name
        while (true) {
            tag = rlfs_dir_fetchmatch(lfs, dir, dir->tail,
                    RLFS_MKTAG(0x780, 0, 0),
                    RLFS_MKTAG(RLFS_TYPE_NAME, 0, namelen),
                    id,
                    rlfs_dir_find_match, &(struct rlfs_dir_find_match){
                        lfs, name, namelen});
            if (tag < 0) {
                return tag;
            }

            if (tag) {
                break;
            }

            if (!dir->split) {
                return RLFS_ERR_NOENT;
            }
        }

        // to next name
        name += namelen;
    }
}

// commit logic
struct rlfs_commit {
    rlfs_block_t block;
    rlfs_off_t off;
    rlfs_tag_t ptag;
    uint32_t crc;

    rlfs_off_t begin;
    rlfs_off_t end;
};

#ifndef RLFS_READONLY
static int rlfs_dir_commitprog(rlfs_t *lfs, struct rlfs_commit *commit,
        const void *buffer, rlfs_size_t size) {
    int err = rlfs_bd_prog(lfs,
            &lfs->pcache, &lfs->rcache, false,
            commit->block, commit->off ,
            (const uint8_t*)buffer, size);
    if (err) {
        return err;
    }

    commit->crc = rlfs_crc(commit->crc, buffer, size);
    commit->off += size;
    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_commitattr(rlfs_t *lfs, struct rlfs_commit *commit,
        rlfs_tag_t tag, const void *buffer) {
    // check if we fit
    rlfs_size_t dsize = rlfs_tag_dsize(tag);
    if (commit->off + dsize > commit->end) {
        return RLFS_ERR_NOSPC;
    }

    // write out tag
    rlfs_tag_t ntag = rlfs_tobe32((tag & 0x7fffffff) ^ commit->ptag);
    int err = rlfs_dir_commitprog(lfs, commit, &ntag, sizeof(ntag));
    if (err) {
        return err;
    }

    if (!(tag & 0x80000000)) {
        // from memory
        err = rlfs_dir_commitprog(lfs, commit, buffer, dsize-sizeof(tag));
        if (err) {
            return err;
        }
    } else {
        // from disk
        const struct rlfs_diskoff *disk = buffer;
        for (rlfs_off_t i = 0; i < dsize-sizeof(tag); i++) {
            // rely on caching to make this efficient
            uint8_t dat;
            err = rlfs_bd_read(lfs,
                    NULL, &lfs->rcache, dsize-sizeof(tag)-i,
                    disk->block, disk->off+i, &dat, 1);
            if (err) {
                return err;
            }

            err = rlfs_dir_commitprog(lfs, commit, &dat, 1);
            if (err) {
                return err;
            }
        }
    }

    commit->ptag = tag & 0x7fffffff;
    return 0;
}
#endif

#ifndef RLFS_READONLY

static int rlfs_dir_commitcrc(rlfs_t *lfs, struct rlfs_commit *commit) {
    // align to program units
    //
    // this gets a bit complex as we have two types of crcs:
    // - 5-word crc with fcrc to check following prog (middle of block)
    // - 2-word crc with no following prog (end of block)
    const rlfs_off_t end = rlfs_alignup(
            rlfs_min(commit->off + 5*sizeof(uint32_t), lfs->cfg->block_size),
            lfs->cfg->prog_size);

    rlfs_off_t off1 = 0;
    uint32_t crc1 = 0;

    // create crc tags to fill up remainder of commit, note that
    // padding is not crced, which lets fetches skip padding but
    // makes committing a bit more complicated
    while (commit->off < end) {
        rlfs_off_t noff = (
                rlfs_min(end - (commit->off+sizeof(rlfs_tag_t)), 0x3fe)
                + (commit->off+sizeof(rlfs_tag_t)));
        // too large for crc tag? need padding commits
        if (noff < end) {
            noff = rlfs_min(noff, end - 5*sizeof(uint32_t));
        }

        // space for fcrc?
        uint8_t eperturb = (uint8_t)-1;
        if (noff >= end && noff <= lfs->cfg->block_size - lfs->cfg->prog_size) {
            // first read the leading byte, this always contains a bit
            // we can perturb to avoid writes that don't change the fcrc
            int err = rlfs_bd_read(lfs,
                    NULL, &lfs->rcache, lfs->cfg->prog_size,
                    commit->block, noff, &eperturb, 1);
            if (err && err != RLFS_ERR_CORRUPT) {
                return err;
            }

        #ifdef RLFS_MULTIVERSION
            // unfortunately fcrcs break mdir fetching < lfs2.1, so only write
            // these if we're a >= lfs2.1 filesystem
            if (rlfs_fs_disk_version(lfs) <= 0x00020000) {
                // don't write fcrc
            } else
        #endif
            {
                // find the expected fcrc, don't bother avoiding a reread
                // of the eperturb, it should still be in our cache
                struct rlfs_fcrc fcrc = {
                    .size = lfs->cfg->prog_size,
                    .crc = 0xffffffff
                };
                err = rlfs_bd_crc(lfs,
                        NULL, &lfs->rcache, lfs->cfg->prog_size,
                        commit->block, noff, fcrc.size, &fcrc.crc);
                if (err && err != RLFS_ERR_CORRUPT) {
                    return err;
                }

                rlfs_fcrc_tole32(&fcrc);
                err = rlfs_dir_commitattr(lfs, commit,
                        RLFS_MKTAG(RLFS_TYPE_FCRC, 0x3ff, sizeof(struct rlfs_fcrc)),
                        &fcrc);
                if (err) {
                    return err;
                }
            }
        }

        // build commit crc
        struct {
            rlfs_tag_t tag;
            uint32_t crc;
        } ccrc;
        rlfs_tag_t ntag = RLFS_MKTAG(
                RLFS_TYPE_CCRC + (((uint8_t)~eperturb) >> 7), 0x3ff,
                noff - (commit->off+sizeof(rlfs_tag_t)));
        ccrc.tag = rlfs_tobe32(ntag ^ commit->ptag);
        commit->crc = rlfs_crc(commit->crc, &ccrc.tag, sizeof(rlfs_tag_t));
        ccrc.crc = rlfs_tole32(commit->crc);

        int err = rlfs_bd_prog(lfs,
                &lfs->pcache, &lfs->rcache, false,
                commit->block, commit->off, &ccrc, sizeof(ccrc));
        if (err) {
            return err;
        }

        // keep track of non-padding checksum to verify
        if (off1 == 0) {
            off1 = commit->off + sizeof(rlfs_tag_t);
            crc1 = commit->crc;
        }

        commit->off = noff;
        // perturb valid bit?
        commit->ptag = ntag ^ ((0x80UL & ~eperturb) << 24);
        // reset crc for next commit
        commit->crc = 0xffffffff;

        // manually flush here since we don't prog the padding, this confuses
        // the caching layer
        if (noff >= end || noff >= lfs->pcache.off + lfs->cfg->cache_size) {
            // flush buffers
            int err = rlfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
            if (err) {
                return err;
            }
        }
    }

    // successful commit, check checksums to make sure
    //
    // note that we don't need to check padding commits, worst
    // case if they are corrupted we would have had to compact anyways
    rlfs_off_t off = commit->begin;
    uint32_t crc = 0xffffffff;
    int err = rlfs_bd_crc(lfs,
            NULL, &lfs->rcache, off1+sizeof(uint32_t),
            commit->block, off, off1-off, &crc);
    if (err) {
        return err;
    }

    // check non-padding commits against known crc
    if (crc != crc1) {
        return RLFS_ERR_CORRUPT;
    }

    // make sure to check crc in case we happen to pick
    // up an unrelated crc (frozen block?)
    err = rlfs_bd_crc(lfs,
            NULL, &lfs->rcache, sizeof(uint32_t),
            commit->block, off1, sizeof(uint32_t), &crc);
    if (err) {
        return err;
    }

    if (crc != 0) {
        return RLFS_ERR_CORRUPT;
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_alloc(rlfs_t *lfs, rlfs_mdir_t *dir) {
    // allocate pair of dir blocks (backwards, so we write block 1 first)
    for (int i = 0; i < 2; i++) {
        int err = rlfs_alloc(lfs, &dir->pair[(i+1)%2]);
        if (err) {
            return err;
        }
    }

    // zero for reproducibility in case initial block is unreadable
    dir->rev = 0;

    // rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = rlfs_bd_read(lfs,
            NULL, &lfs->rcache, sizeof(dir->rev),
            dir->pair[0], 0, &dir->rev, sizeof(dir->rev));
    dir->rev = rlfs_fromle32(dir->rev);
    if (err && err != RLFS_ERR_CORRUPT) {
        return err;
    }

    // to make sure we don't immediately evict, align the new revision count
    // to our block_cycles modulus, see rlfs_dir_compact for why our modulus
    // is tweaked this way
    if (lfs->cfg->block_cycles > 0) {
        dir->rev = rlfs_alignup(dir->rev, ((lfs->cfg->block_cycles+1)|1));
    }

    // set defaults
    dir->off = sizeof(dir->rev);
    dir->etag = 0xffffffff;
    dir->count = 0;
    dir->tail[0] = RLFS_BLOCK_NULL;
    dir->tail[1] = RLFS_BLOCK_NULL;
    dir->erased = false;
    dir->split = false;

    // don't write out yet, let caller take care of that
    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_drop(rlfs_t *lfs, rlfs_mdir_t *dir, rlfs_mdir_t *tail) {
    // steal state
    int err = rlfs_dir_getgstate(lfs, tail, &lfs->gdelta);
    if (err) {
        return err;
    }

    // steal tail
    rlfs_pair_tole32(tail->tail);
    err = rlfs_dir_commit(lfs, dir, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_TAIL + tail->split, 0x3ff, 8), tail->tail}));
    rlfs_pair_fromle32(tail->tail);
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_split(rlfs_t *lfs,
        rlfs_mdir_t *dir, const struct rlfs_mattr *attrs, int attrcount,
        rlfs_mdir_t *source, uint16_t split, uint16_t end) {
    // create tail metadata pair
    rlfs_mdir_t tail;
    int err = rlfs_dir_alloc(lfs, &tail);
    if (err) {
        return err;
    }

    tail.split = dir->split;
    tail.tail[0] = dir->tail[0];
    tail.tail[1] = dir->tail[1];

    // note we don't care about RLFS_OK_RELOCATED
    int res = rlfs_dir_compact(lfs, &tail, attrs, attrcount, source, split, end);
    if (res < 0) {
        return res;
    }

    dir->tail[0] = tail.pair[0];
    dir->tail[1] = tail.pair[1];
    dir->split = true;

    // update root if needed
    if (rlfs_pair_cmp(dir->pair, lfs->root) == 0 && split == 0) {
        lfs->root[0] = tail.pair[0];
        lfs->root[1] = tail.pair[1];
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_commit_size(void *p, rlfs_tag_t tag, const void *buffer) {
    rlfs_size_t *size = p;
    (void)buffer;

    *size += rlfs_tag_dsize(tag);
    return 0;
}
#endif

#ifndef RLFS_READONLY
struct rlfs_dir_commit_commit {
    rlfs_t *lfs;
    struct rlfs_commit *commit;
};
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_commit_commit(void *p, rlfs_tag_t tag, const void *buffer) {
    struct rlfs_dir_commit_commit *commit = p;
    return rlfs_dir_commitattr(commit->lfs, commit->commit, tag, buffer);
}
#endif

#ifndef RLFS_READONLY
static bool rlfs_dir_needsrelocation(rlfs_t *lfs, rlfs_mdir_t *dir) {
    // If our revision count == n * block_cycles, we should force a relocation,
    // this is how littlefs wear-levels at the metadata-pair level. Note that we
    // actually use (block_cycles+1)|1, this is to avoid two corner cases:
    // 1. block_cycles = 1, which would prevent relocations from terminating
    // 2. block_cycles = 2n, which, due to aliasing, would only ever relocate
    //    one metadata block in the pair, effectively making this useless
    return (lfs->cfg->block_cycles > 0
            && ((dir->rev + 1) % ((lfs->cfg->block_cycles+1)|1) == 0));
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_compact(rlfs_t *lfs,
        rlfs_mdir_t *dir, const struct rlfs_mattr *attrs, int attrcount,
        rlfs_mdir_t *source, uint16_t begin, uint16_t end) {
    // save some state in case block is bad
    bool relocated = false;
    bool tired = rlfs_dir_needsrelocation(lfs, dir);

    // increment revision count
    dir->rev += 1;

    // do not proactively relocate blocks during migrations, this
    // can cause a number of failure states such: clobbering the
    // v1 superblock if we relocate root, and invalidating directory
    // pointers if we relocate the head of a directory. On top of
    // this, relocations increase the overall complexity of
    // rlfs_migration, which is already a delicate operation.
#ifdef RLFS_MIGRATE
    if (lfs->lfs1) {
        tired = false;
    }
#endif

    if (tired && rlfs_pair_cmp(dir->pair, (const rlfs_block_t[2]){0, 1}) != 0) {
        // we're writing too much, time to relocate
        goto relocate;
    }

    // begin loop to commit compaction to blocks until a compact sticks
    while (true) {
        {
            // setup commit state
            struct rlfs_commit commit = {
                .block = dir->pair[1],
                .off = 0,
                .ptag = 0xffffffff,
                .crc = 0xffffffff,

                .begin = 0,
                .end = (lfs->cfg->metadata_max ?
                    lfs->cfg->metadata_max : lfs->cfg->block_size) - 8,
            };

            // erase block to write to
            int err = rlfs_bd_erase(lfs, dir->pair[1]);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // write out header
            dir->rev = rlfs_tole32(dir->rev);
            err = rlfs_dir_commitprog(lfs, &commit,
                    &dir->rev, sizeof(dir->rev));
            dir->rev = rlfs_fromle32(dir->rev);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // traverse the directory, this time writing out all unique tags
            err = rlfs_dir_traverse(lfs,
                    source, 0, 0xffffffff, attrs, attrcount,
                    RLFS_MKTAG(0x400, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_NAME, 0, 0),
                    begin, end, -begin,
                    rlfs_dir_commit_commit, &(struct rlfs_dir_commit_commit){
                        lfs, &commit});
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // commit tail, which may be new after last size check
            if (!rlfs_pair_isnull(dir->tail)) {
                rlfs_pair_tole32(dir->tail);
                err = rlfs_dir_commitattr(lfs, &commit,
                        RLFS_MKTAG(RLFS_TYPE_TAIL + dir->split, 0x3ff, 8),
                        dir->tail);
                rlfs_pair_fromle32(dir->tail);
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            // bring over gstate?
            rlfs_gstate_t delta = {0};
            if (!relocated) {
                rlfs_gstate_xor(&delta, &lfs->gdisk);
                rlfs_gstate_xor(&delta, &lfs->gstate);
            }
            rlfs_gstate_xor(&delta, &lfs->gdelta);
            delta.tag &= ~RLFS_MKTAG(0, 0, 0x3ff);

            err = rlfs_dir_getgstate(lfs, dir, &delta);
            if (err) {
                return err;
            }

            if (!rlfs_gstate_iszero(&delta)) {
                rlfs_gstate_tole32(&delta);
                err = rlfs_dir_commitattr(lfs, &commit,
                        RLFS_MKTAG(RLFS_TYPE_MOVESTATE, 0x3ff,
                            sizeof(delta)), &delta);
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            // complete commit with crc
            err = rlfs_dir_commitcrc(lfs, &commit);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // successful compaction, swap dir pair to indicate most recent
            RLFS_ASSERT(commit.off % lfs->cfg->prog_size == 0);
            rlfs_pair_swap(dir->pair);
            dir->count = end - begin;
            dir->off = commit.off;
            dir->etag = commit.ptag;
            // update gstate
            lfs->gdelta = (rlfs_gstate_t){0};
            if (!relocated) {
                lfs->gdisk = lfs->gstate;
            }
        }
        break;

relocate:
        // commit was corrupted, drop caches and prepare to relocate block
        relocated = true;
        rlfs_cache_drop(lfs, &lfs->pcache);
        if (!tired) {
            RLFS_DEBUG("Bad block at 0x%"PRIx32, dir->pair[1]);
        }

        // can't relocate superblock, filesystem is now frozen
        if (rlfs_pair_cmp(dir->pair, (const rlfs_block_t[2]){0, 1}) == 0) {
            RLFS_WARN("Superblock 0x%"PRIx32" has become unwritable",
                    dir->pair[1]);
            return RLFS_ERR_NOSPC;
        }

        // relocate half of pair
        int err = rlfs_alloc(lfs, &dir->pair[1]);
        if (err && (err != RLFS_ERR_NOSPC || !tired)) {
            return err;
        }

        tired = false;
        continue;
    }

    return relocated ? RLFS_OK_RELOCATED : 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_splittingcompact(rlfs_t *lfs, rlfs_mdir_t *dir,
        const struct rlfs_mattr *attrs, int attrcount,
        rlfs_mdir_t *source, uint16_t begin, uint16_t end) {
    while (true) {
        // find size of first split, we do this by halving the split until
        // the metadata is guaranteed to fit
        //
        // Note that this isn't a true binary search, we never increase the
        // split size. This may result in poorly distributed metadata but isn't
        // worth the extra code size or performance hit to fix.
        rlfs_size_t split = begin;
        while (end - split > 1) {
            rlfs_size_t size = 0;
            int err = rlfs_dir_traverse(lfs,
                    source, 0, 0xffffffff, attrs, attrcount,
                    RLFS_MKTAG(0x400, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_NAME, 0, 0),
                    split, end, -split,
                    rlfs_dir_commit_size, &size);
            if (err) {
                return err;
            }

            // space is complicated, we need room for:
            //
            // - tail:         4+2*4 = 12 bytes
            // - gstate:       4+3*4 = 16 bytes
            // - move delete:  4     = 4 bytes
            // - crc:          4+4   = 8 bytes
            //                 total = 40 bytes
            //
            // And we cap at half a block to avoid degenerate cases with
            // nearly-full metadata blocks.
            //
            rlfs_size_t metadata_max = (lfs->cfg->metadata_max)
                    ? lfs->cfg->metadata_max
                    : lfs->cfg->block_size;
            if (end - split < 0xff
                    && size <= rlfs_min(
                        metadata_max - 40,
                        rlfs_alignup(
                            metadata_max/2,
                            lfs->cfg->prog_size))) {
                break;
            }

            split = split + ((end - split) / 2);
        }

        if (split == begin) {
            // no split needed
            break;
        }

        // split into two metadata pairs and continue
        int err = rlfs_dir_split(lfs, dir, attrs, attrcount,
                source, split, end);
        if (err && err != RLFS_ERR_NOSPC) {
            return err;
        }

        if (err) {
            // we can't allocate a new block, try to compact with degraded
            // performance
            RLFS_WARN("Unable to split {0x%"PRIx32", 0x%"PRIx32"}",
                    dir->pair[0], dir->pair[1]);
            break;
        } else {
            end = split;
        }
    }

    if (rlfs_dir_needsrelocation(lfs, dir)
            && rlfs_pair_cmp(dir->pair, (const rlfs_block_t[2]){0, 1}) == 0) {
        // oh no! we're writing too much to the superblock,
        // should we expand?
        rlfs_ssize_t size = rlfs_fs_size_(lfs);
        if (size < 0) {
            return size;
        }

        // littlefs cannot reclaim expanded superblocks, so expand cautiously
        //
        // if our filesystem is more than ~88% full, don't expand, this is
        // somewhat arbitrary
        if (lfs->block_count - size > lfs->block_count/8) {
            RLFS_DEBUG("Expanding superblock at rev %"PRIu32, dir->rev);
            int err = rlfs_dir_split(lfs, dir, attrs, attrcount,
                    source, begin, end);
            if (err && err != RLFS_ERR_NOSPC) {
                return err;
            }

            if (err) {
                // welp, we tried, if we ran out of space there's not much
                // we can do, we'll error later if we've become frozen
                RLFS_WARN("Unable to expand superblock");
            } else {
                // duplicate the superblock entry into the new superblock
                end = 1;
            }
        }
    }

    return rlfs_dir_compact(lfs, dir, attrs, attrcount, source, begin, end);
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_relocatingcommit(rlfs_t *lfs, rlfs_mdir_t *dir,
        const rlfs_block_t pair[2],
        const struct rlfs_mattr *attrs, int attrcount,
        rlfs_mdir_t *pdir) {
    int state = 0;

    // calculate changes to the directory
    bool hasdelete = false;
    for (int i = 0; i < attrcount; i++) {
        if (rlfs_tag_type3(attrs[i].tag) == RLFS_TYPE_CREATE) {
            dir->count += 1;
        } else if (rlfs_tag_type3(attrs[i].tag) == RLFS_TYPE_DELETE) {
            RLFS_ASSERT(dir->count > 0);
            dir->count -= 1;
            hasdelete = true;
        } else if (rlfs_tag_type1(attrs[i].tag) == RLFS_TYPE_TAIL) {
            dir->tail[0] = ((rlfs_block_t*)attrs[i].buffer)[0];
            dir->tail[1] = ((rlfs_block_t*)attrs[i].buffer)[1];
            dir->split = (rlfs_tag_chunk(attrs[i].tag) & 1);
            rlfs_pair_fromle32(dir->tail);
        }
    }

    // should we actually drop the directory block?
    if (hasdelete && dir->count == 0) {
        RLFS_ASSERT(pdir);
        int err = rlfs_fs_pred(lfs, dir->pair, pdir);
        if (err && err != RLFS_ERR_NOENT) {
            return err;
        }

        if (err != RLFS_ERR_NOENT && pdir->split) {
            state = RLFS_OK_DROPPED;
            goto fixmlist;
        }
    }

    if (dir->erased && dir->count < 0xff) {
        // try to commit
        struct rlfs_commit commit = {
            .block = dir->pair[0],
            .off = dir->off,
            .ptag = dir->etag,
            .crc = 0xffffffff,

            .begin = dir->off,
            .end = (lfs->cfg->metadata_max ?
                lfs->cfg->metadata_max : lfs->cfg->block_size) - 8,
        };

        // traverse attrs that need to be written out
        rlfs_pair_tole32(dir->tail);
        int err = rlfs_dir_traverse(lfs,
                dir, dir->off, dir->etag, attrs, attrcount,
                0, 0, 0, 0, 0,
                rlfs_dir_commit_commit, &(struct rlfs_dir_commit_commit){
                    lfs, &commit});
        rlfs_pair_fromle32(dir->tail);
        if (err) {
            if (err == RLFS_ERR_NOSPC || err == RLFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        // commit any global diffs if we have any
        rlfs_gstate_t delta = {0};
        rlfs_gstate_xor(&delta, &lfs->gstate);
        rlfs_gstate_xor(&delta, &lfs->gdisk);
        rlfs_gstate_xor(&delta, &lfs->gdelta);
        delta.tag &= ~RLFS_MKTAG(0, 0, 0x3ff);
        if (!rlfs_gstate_iszero(&delta)) {
            err = rlfs_dir_getgstate(lfs, dir, &delta);
            if (err) {
                return err;
            }

            rlfs_gstate_tole32(&delta);
            err = rlfs_dir_commitattr(lfs, &commit,
                    RLFS_MKTAG(RLFS_TYPE_MOVESTATE, 0x3ff,
                        sizeof(delta)), &delta);
            if (err) {
                if (err == RLFS_ERR_NOSPC || err == RLFS_ERR_CORRUPT) {
                    goto compact;
                }
                return err;
            }
        }

        // finalize commit with the crc
        err = rlfs_dir_commitcrc(lfs, &commit);
        if (err) {
            if (err == RLFS_ERR_NOSPC || err == RLFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        // successful commit, update dir
        RLFS_ASSERT(commit.off % lfs->cfg->prog_size == 0);
        dir->off = commit.off;
        dir->etag = commit.ptag;
        // and update gstate
        lfs->gdisk = lfs->gstate;
        lfs->gdelta = (rlfs_gstate_t){0};

        goto fixmlist;
    }

compact:
    // fall back to compaction
    rlfs_cache_drop(lfs, &lfs->pcache);

    state = rlfs_dir_splittingcompact(lfs, dir, attrs, attrcount,
            dir, 0, dir->count);
    if (state < 0) {
        return state;
    }

    goto fixmlist;

fixmlist:;
    // this complicated bit of logic is for fixing up any active
    // metadata-pairs that we may have affected
    //
    // note we have to make two passes since the mdir passed to
    // rlfs_dir_commit could also be in this list, and even then
    // we need to copy the pair so they don't get clobbered if we refetch
    // our mdir.
    rlfs_block_t oldpair[2] = {pair[0], pair[1]};
    for (struct rlfs_mlist *d = lfs->mlist; d; d = d->next) {
        if (rlfs_pair_cmp(d->m.pair, oldpair) == 0) {
            d->m = *dir;
            if (d->m.pair != pair) {
                for (int i = 0; i < attrcount; i++) {
                    if (rlfs_tag_type3(attrs[i].tag) == RLFS_TYPE_DELETE &&
                            d->id == rlfs_tag_id(attrs[i].tag) &&
                            d->type != RLFS_TYPE_DIR) {
                        d->m.pair[0] = RLFS_BLOCK_NULL;
                        d->m.pair[1] = RLFS_BLOCK_NULL;
                    } else if (rlfs_tag_type3(attrs[i].tag) == RLFS_TYPE_DELETE &&
                            d->id > rlfs_tag_id(attrs[i].tag)) {
                        d->id -= 1;
                        if (d->type == RLFS_TYPE_DIR) {
                            ((rlfs_dir_t*)d)->pos -= 1;
                        }
                    } else if (rlfs_tag_type3(attrs[i].tag) == RLFS_TYPE_CREATE &&
                            d->id >= rlfs_tag_id(attrs[i].tag)) {
                        d->id += 1;
                        if (d->type == RLFS_TYPE_DIR) {
                            ((rlfs_dir_t*)d)->pos += 1;
                        }
                    }
                }
            }

            while (d->id >= d->m.count && d->m.split) {
                // we split and id is on tail now
                if (rlfs_pair_cmp(d->m.tail, lfs->root) != 0) {
                    d->id -= d->m.count;
                }
                int err = rlfs_dir_fetch(lfs, &d->m, d->m.tail);
                if (err) {
                    return err;
                }
            }
        }
    }

    return state;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_orphaningcommit(rlfs_t *lfs, rlfs_mdir_t *dir,
        const struct rlfs_mattr *attrs, int attrcount) {
    // check for any inline files that aren't RAM backed and
    // forcefully evict them, needed for filesystem consistency
    for (rlfs_file_t *f = (rlfs_file_t*)lfs->mlist; f; f = f->next) {
        if (dir != &f->m && rlfs_pair_cmp(f->m.pair, dir->pair) == 0 &&
                f->type == RLFS_TYPE_REG && (f->flags & RLFS_F_INLINE) &&
                f->ctz.size > lfs->cfg->cache_size) {
            int err = rlfs_file_outline(lfs, f);
            if (err) {
                return err;
            }

            err = rlfs_file_flush(lfs, f);
            if (err) {
                return err;
            }
        }
    }

    rlfs_block_t lpair[2] = {dir->pair[0], dir->pair[1]};
    rlfs_mdir_t ldir = *dir;
    rlfs_mdir_t pdir;
    int state = rlfs_dir_relocatingcommit(lfs, &ldir, dir->pair,
            attrs, attrcount, &pdir);
    if (state < 0) {
        return state;
    }

    // update if we're not in mlist, note we may have already been
    // updated if we are in mlist
    if (rlfs_pair_cmp(dir->pair, lpair) == 0) {
        *dir = ldir;
    }

    // commit was successful, but may require other changes in the
    // filesystem, these would normally be tail recursive, but we have
    // flattened them here avoid unbounded stack usage

    // need to drop?
    if (state == RLFS_OK_DROPPED) {
        // steal state
        int err = rlfs_dir_getgstate(lfs, dir, &lfs->gdelta);
        if (err) {
            return err;
        }

        // steal tail, note that this can't create a recursive drop
        lpair[0] = pdir.pair[0];
        lpair[1] = pdir.pair[1];
        rlfs_pair_tole32(dir->tail);
        state = rlfs_dir_relocatingcommit(lfs, &pdir, lpair, RLFS_MKATTRS(
                    {RLFS_MKTAG(RLFS_TYPE_TAIL + dir->split, 0x3ff, 8),
                        dir->tail}),
                NULL);
        rlfs_pair_fromle32(dir->tail);
        if (state < 0) {
            return state;
        }

        ldir = pdir;
    }

    // need to relocate?
    bool orphans = false;
    while (state == RLFS_OK_RELOCATED) {
        RLFS_DEBUG("Relocating {0x%"PRIx32", 0x%"PRIx32"} "
                    "-> {0x%"PRIx32", 0x%"PRIx32"}",
                lpair[0], lpair[1], ldir.pair[0], ldir.pair[1]);
        state = 0;

        // update internal root
        if (rlfs_pair_cmp(lpair, lfs->root) == 0) {
            lfs->root[0] = ldir.pair[0];
            lfs->root[1] = ldir.pair[1];
        }

        // update internally tracked dirs
        for (struct rlfs_mlist *d = lfs->mlist; d; d = d->next) {
            if (rlfs_pair_cmp(lpair, d->m.pair) == 0) {
                d->m.pair[0] = ldir.pair[0];
                d->m.pair[1] = ldir.pair[1];
            }

            if (d->type == RLFS_TYPE_DIR &&
                    rlfs_pair_cmp(lpair, ((rlfs_dir_t*)d)->head) == 0) {
                ((rlfs_dir_t*)d)->head[0] = ldir.pair[0];
                ((rlfs_dir_t*)d)->head[1] = ldir.pair[1];
            }
        }

        // find parent
        rlfs_stag_t tag = rlfs_fs_parent(lfs, lpair, &pdir);
        if (tag < 0 && tag != RLFS_ERR_NOENT) {
            return tag;
        }

        bool hasparent = (tag != RLFS_ERR_NOENT);
        if (tag != RLFS_ERR_NOENT) {
            // note that if we have a parent, we must have a pred, so this will
            // always create an orphan
            int err = rlfs_fs_preporphans(lfs, +1);
            if (err) {
                return err;
            }

            // fix pending move in this pair? this looks like an optimization but
            // is in fact _required_ since relocating may outdate the move.
            uint16_t moveid = 0x3ff;
            if (rlfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
                moveid = rlfs_tag_id(lfs->gstate.tag);
                RLFS_DEBUG("Fixing move while relocating "
                        "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
                        pdir.pair[0], pdir.pair[1], moveid);
                rlfs_fs_prepmove(lfs, 0x3ff, NULL);
                if (moveid < rlfs_tag_id(tag)) {
                    tag -= RLFS_MKTAG(0, 1, 0);
                }
            }

            rlfs_block_t ppair[2] = {pdir.pair[0], pdir.pair[1]};
            rlfs_pair_tole32(ldir.pair);
            state = rlfs_dir_relocatingcommit(lfs, &pdir, ppair, RLFS_MKATTRS(
                        {RLFS_MKTAG_IF(moveid != 0x3ff,
                            RLFS_TYPE_DELETE, moveid, 0), NULL},
                        {tag, ldir.pair}),
                    NULL);
            rlfs_pair_fromle32(ldir.pair);
            if (state < 0) {
                return state;
            }

            if (state == RLFS_OK_RELOCATED) {
                lpair[0] = ppair[0];
                lpair[1] = ppair[1];
                ldir = pdir;
                orphans = true;
                continue;
            }
        }

        // find pred
        int err = rlfs_fs_pred(lfs, lpair, &pdir);
        if (err && err != RLFS_ERR_NOENT) {
            return err;
        }
        RLFS_ASSERT(!(hasparent && err == RLFS_ERR_NOENT));

        // if we can't find dir, it must be new
        if (err != RLFS_ERR_NOENT) {
            if (rlfs_gstate_hasorphans(&lfs->gstate)) {
                // next step, clean up orphans
                err = rlfs_fs_preporphans(lfs, -(int8_t)hasparent);
                if (err) {
                    return err;
                }
            }

            // fix pending move in this pair? this looks like an optimization
            // but is in fact _required_ since relocating may outdate the move.
            uint16_t moveid = 0x3ff;
            if (rlfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
                moveid = rlfs_tag_id(lfs->gstate.tag);
                RLFS_DEBUG("Fixing move while relocating "
                        "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
                        pdir.pair[0], pdir.pair[1], moveid);
                rlfs_fs_prepmove(lfs, 0x3ff, NULL);
            }

            // replace bad pair, either we clean up desync, or no desync occured
            lpair[0] = pdir.pair[0];
            lpair[1] = pdir.pair[1];
            rlfs_pair_tole32(ldir.pair);
            state = rlfs_dir_relocatingcommit(lfs, &pdir, lpair, RLFS_MKATTRS(
                        {RLFS_MKTAG_IF(moveid != 0x3ff,
                            RLFS_TYPE_DELETE, moveid, 0), NULL},
                        {RLFS_MKTAG(RLFS_TYPE_TAIL + pdir.split, 0x3ff, 8),
                            ldir.pair}),
                    NULL);
            rlfs_pair_fromle32(ldir.pair);
            if (state < 0) {
                return state;
            }

            ldir = pdir;
        }
    }

    return orphans ? RLFS_OK_ORPHANED : 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_dir_commit(rlfs_t *lfs, rlfs_mdir_t *dir,
        const struct rlfs_mattr *attrs, int attrcount) {
    int orphans = rlfs_dir_orphaningcommit(lfs, dir, attrs, attrcount);
    if (orphans < 0) {
        return orphans;
    }

    if (orphans) {
        // make sure we've removed all orphans, this is a noop if there
        // are none, but if we had nested blocks failures we may have
        // created some
        int err = rlfs_fs_deorphan(lfs, false);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif


/// Top level directory operations ///
#ifndef RLFS_READONLY
static int rlfs_mkdir_(rlfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = rlfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    struct rlfs_mlist cwd;
    cwd.next = lfs->mlist;
    uint16_t id;
    err = rlfs_dir_find(lfs, &cwd.m, &path, &id);
    if (!(err == RLFS_ERR_NOENT && rlfs_path_islast(path))) {
        return (err < 0) ? err : RLFS_ERR_EXIST;
    }

    // check that name fits
    rlfs_size_t nlen = rlfs_path_namelen(path);
    if (nlen > lfs->name_max) {
        return RLFS_ERR_NAMETOOLONG;
    }

    // build up new directory
    rlfs_alloc_ckpoint(lfs);
    rlfs_mdir_t dir;
    err = rlfs_dir_alloc(lfs, &dir);
    if (err) {
        return err;
    }

    // find end of list
    rlfs_mdir_t pred = cwd.m;
    while (pred.split) {
        err = rlfs_dir_fetch(lfs, &pred, pred.tail);
        if (err) {
            return err;
        }
    }

    // setup dir
    rlfs_pair_tole32(pred.tail);
    err = rlfs_dir_commit(lfs, &dir, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_SOFTTAIL, 0x3ff, 8), pred.tail}));
    rlfs_pair_fromle32(pred.tail);
    if (err) {
        return err;
    }

    // current block not end of list?
    if (cwd.m.split) {
        // update tails, this creates a desync
        err = rlfs_fs_preporphans(lfs, +1);
        if (err) {
            return err;
        }

        // it's possible our predecessor has to be relocated, and if
        // our parent is our predecessor's predecessor, this could have
        // caused our parent to go out of date, fortunately we can hook
        // ourselves into littlefs to catch this
        cwd.type = 0;
        cwd.id = 0;
        lfs->mlist = &cwd;

        rlfs_pair_tole32(dir.pair);
        err = rlfs_dir_commit(lfs, &pred, RLFS_MKATTRS(
                {RLFS_MKTAG(RLFS_TYPE_SOFTTAIL, 0x3ff, 8), dir.pair}));
        rlfs_pair_fromle32(dir.pair);
        if (err) {
            lfs->mlist = cwd.next;
            return err;
        }

        lfs->mlist = cwd.next;
        err = rlfs_fs_preporphans(lfs, -1);
        if (err) {
            return err;
        }
    }

    // now insert into our parent block
    rlfs_pair_tole32(dir.pair);
    err = rlfs_dir_commit(lfs, &cwd.m, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_CREATE, id, 0), NULL},
            {RLFS_MKTAG(RLFS_TYPE_DIR, id, nlen), path},
            {RLFS_MKTAG(RLFS_TYPE_DIRSTRUCT, id, 8), dir.pair},
            {RLFS_MKTAG_IF(!cwd.m.split,
                RLFS_TYPE_SOFTTAIL, 0x3ff, 8), dir.pair}));
    rlfs_pair_fromle32(dir.pair);
    if (err) {
        return err;
    }

    return 0;
}
#endif

static int rlfs_dir_open_(rlfs_t *lfs, rlfs_dir_t *dir, const char *path) {
    rlfs_stag_t tag = rlfs_dir_find(lfs, &dir->m, &path, NULL);
    if (tag < 0) {
        return tag;
    }

    if (rlfs_tag_type3(tag) != RLFS_TYPE_DIR) {
        return RLFS_ERR_NOTDIR;
    }

    rlfs_block_t pair[2];
    if (rlfs_tag_id(tag) == 0x3ff) {
        // handle root dir separately
        pair[0] = lfs->root[0];
        pair[1] = lfs->root[1];
    } else {
        // get dir pair from parent
        rlfs_stag_t res = rlfs_dir_get(lfs, &dir->m, RLFS_MKTAG(0x700, 0x3ff, 0),
                RLFS_MKTAG(RLFS_TYPE_STRUCT, rlfs_tag_id(tag), 8), pair);
        if (res < 0) {
            return res;
        }
        rlfs_pair_fromle32(pair);
    }

    // fetch first pair
    int err = rlfs_dir_fetch(lfs, &dir->m, pair);
    if (err) {
        return err;
    }

    // setup entry
    dir->head[0] = dir->m.pair[0];
    dir->head[1] = dir->m.pair[1];
    dir->id = 0;
    dir->pos = 0;

    // add to list of mdirs
    dir->type = RLFS_TYPE_DIR;
    rlfs_mlist_append(lfs, (struct rlfs_mlist *)dir);

    return 0;
}

static int rlfs_dir_close_(rlfs_t *lfs, rlfs_dir_t *dir) {
    // remove from list of mdirs
    rlfs_mlist_remove(lfs, (struct rlfs_mlist *)dir);

    return 0;
}

static int rlfs_dir_read_(rlfs_t *lfs, rlfs_dir_t *dir, struct rlfs_info *info) {
    memset(info, 0, sizeof(*info));

    // special offset for '.' and '..'
    if (dir->pos == 0) {
        info->type = RLFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->pos += 1;
        return true;
    } else if (dir->pos == 1) {
        info->type = RLFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->pos += 1;
        return true;
    }

    while (true) {
        if (dir->id == dir->m.count) {
            if (!dir->m.split) {
                return false;
            }

            int err = rlfs_dir_fetch(lfs, &dir->m, dir->m.tail);
            if (err) {
                return err;
            }

            dir->id = 0;
        }

        int err = rlfs_dir_getinfo(lfs, &dir->m, dir->id, info);
        if (err && err != RLFS_ERR_NOENT) {
            return err;
        }

        dir->id += 1;
        if (err != RLFS_ERR_NOENT) {
            break;
        }
    }

    dir->pos += 1;
    return true;
}

static int rlfs_dir_seek_(rlfs_t *lfs, rlfs_dir_t *dir, rlfs_off_t off) {
    // simply walk from head dir
    int err = rlfs_dir_rewind_(lfs, dir);
    if (err) {
        return err;
    }

    // first two for ./..
    dir->pos = rlfs_min(2, off);
    off -= dir->pos;

    // skip superblock entry
    dir->id = (off > 0 && rlfs_pair_cmp(dir->head, lfs->root) == 0);

    while (off > 0) {
        if (dir->id == dir->m.count) {
            if (!dir->m.split) {
                return RLFS_ERR_INVAL;
            }

            err = rlfs_dir_fetch(lfs, &dir->m, dir->m.tail);
            if (err) {
                return err;
            }

            dir->id = 0;
        }

        int diff = rlfs_min(dir->m.count - dir->id, off);
        dir->id += diff;
        dir->pos += diff;
        off -= diff;
    }

    return 0;
}

static rlfs_soff_t rlfs_dir_tell_(rlfs_t *lfs, rlfs_dir_t *dir) {
    (void)lfs;
    return dir->pos;
}

static int rlfs_dir_rewind_(rlfs_t *lfs, rlfs_dir_t *dir) {
    // reload the head dir
    int err = rlfs_dir_fetch(lfs, &dir->m, dir->head);
    if (err) {
        return err;
    }

    dir->id = 0;
    dir->pos = 0;
    return 0;
}


/// File index list operations ///
static int rlfs_ctz_index(rlfs_t *lfs, rlfs_off_t *off) {
    rlfs_off_t size = *off;
    rlfs_off_t b = lfs->cfg->block_size - 2*4;
    rlfs_off_t i = size / b;
    if (i == 0) {
        return 0;
    }

    i = (size - 4*(rlfs_popc(i-1)+2)) / b;
    *off = size - b*i - 4*rlfs_popc(i);
    return i;
}

static int rlfs_ctz_find(rlfs_t *lfs,
        const rlfs_cache_t *pcache, rlfs_cache_t *rcache,
        rlfs_block_t head, rlfs_size_t size,
        rlfs_size_t pos, rlfs_block_t *block, rlfs_off_t *off) {
    if (size == 0) {
        *block = RLFS_BLOCK_NULL;
        *off = 0;
        return 0;
    }

    rlfs_off_t current = rlfs_ctz_index(lfs, &(rlfs_off_t){size-1});
    rlfs_off_t target = rlfs_ctz_index(lfs, &pos);

    while (current > target) {
        rlfs_size_t skip = rlfs_min(
                rlfs_npw2(current-target+1) - 1,
                rlfs_ctz(current));

        int err = rlfs_bd_read(lfs,
                pcache, rcache, sizeof(head),
                head, 4*skip, &head, sizeof(head));
        head = rlfs_fromle32(head);
        if (err) {
            return err;
        }

        current -= 1 << skip;
    }

    *block = head;
    *off = pos;
    return 0;
}

#ifndef RLFS_READONLY
static int rlfs_ctz_extend(rlfs_t *lfs,
        rlfs_cache_t *pcache, rlfs_cache_t *rcache,
        rlfs_block_t head, rlfs_size_t size,
        rlfs_block_t *block, rlfs_off_t *off) {
    while (true) {
        // go ahead and grab a block
        rlfs_block_t nblock;
        int err = rlfs_alloc(lfs, &nblock);
        if (err) {
            return err;
        }

        {
            err = rlfs_bd_erase(lfs, nblock);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            if (size == 0) {
                *block = nblock;
                *off = 0;
                return 0;
            }

            rlfs_size_t noff = size - 1;
            rlfs_off_t index = rlfs_ctz_index(lfs, &noff);
            noff = noff + 1;

            // just copy out the last block if it is incomplete
            if (noff != lfs->cfg->block_size) {
                for (rlfs_off_t i = 0; i < noff; i++) {
                    uint8_t data;
                    err = rlfs_bd_read(lfs,
                            NULL, rcache, noff-i,
                            head, i, &data, 1);
                    if (err) {
                        return err;
                    }

                    err = rlfs_bd_prog(lfs,
                            pcache, rcache, true,
                            nblock, i, &data, 1);
                    if (err) {
                        if (err == RLFS_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }
                }

                *block = nblock;
                *off = noff;
                return 0;
            }

            // append block
            index += 1;
            rlfs_size_t skips = rlfs_ctz(index) + 1;
            rlfs_block_t nhead = head;
            for (rlfs_off_t i = 0; i < skips; i++) {
                nhead = rlfs_tole32(nhead);
                err = rlfs_bd_prog(lfs, pcache, rcache, true,
                        nblock, 4*i, &nhead, 4);
                nhead = rlfs_fromle32(nhead);
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                if (i != skips-1) {
                    err = rlfs_bd_read(lfs,
                            NULL, rcache, sizeof(nhead),
                            nhead, 4*i, &nhead, sizeof(nhead));
                    nhead = rlfs_fromle32(nhead);
                    if (err) {
                        return err;
                    }
                }
            }

            *block = nblock;
            *off = 4*skips;
            return 0;
        }

relocate:
        RLFS_DEBUG("Bad block at 0x%"PRIx32, nblock);

        // just clear cache and try a new block
        rlfs_cache_drop(lfs, pcache);
    }
}
#endif

static int rlfs_ctz_traverse(rlfs_t *lfs,
        const rlfs_cache_t *pcache, rlfs_cache_t *rcache,
        rlfs_block_t head, rlfs_size_t size,
        int (*cb)(void*, rlfs_block_t), void *data) {
    if (size == 0) {
        return 0;
    }

    rlfs_off_t index = rlfs_ctz_index(lfs, &(rlfs_off_t){size-1});

    while (true) {
        int err = cb(data, head);
        if (err) {
            return err;
        }

        if (index == 0) {
            return 0;
        }

        rlfs_block_t heads[2];
        int count = 2 - (index & 1);
        err = rlfs_bd_read(lfs,
                pcache, rcache, count*sizeof(head),
                head, 0, &heads, count*sizeof(head));
        heads[0] = rlfs_fromle32(heads[0]);
        heads[1] = rlfs_fromle32(heads[1]);
        if (err) {
            return err;
        }

        for (int i = 0; i < count-1; i++) {
            err = cb(data, heads[i]);
            if (err) {
                return err;
            }
        }

        head = heads[count-1];
        index -= count;
    }
}


/// Top level file operations ///
static int rlfs_file_opencfg_(rlfs_t *lfs, rlfs_file_t *file,
        const char *path, int flags,
        const struct rlfs_file_config *cfg) {
#ifndef RLFS_READONLY
    // deorphan if we haven't yet, needed at most once after poweron
    if ((flags & RLFS_O_WRONLY) == RLFS_O_WRONLY) {
        int err = rlfs_fs_forceconsistency(lfs);
        if (err) {
            return err;
        }
    }
#else
    RLFS_ASSERT((flags & RLFS_O_RDONLY) == RLFS_O_RDONLY);
#endif

    // setup simple file details
    int err;
    file->cfg = cfg;
    file->flags = flags;
    file->pos = 0;
    file->off = 0;
    file->cache.buffer = NULL;

    // allocate entry for file if it doesn't exist
    rlfs_stag_t tag = rlfs_dir_find(lfs, &file->m, &path, &file->id);
    if (tag < 0 && !(tag == RLFS_ERR_NOENT && rlfs_path_islast(path))) {
        err = tag;
        goto cleanup;
    }

    // get id, add to list of mdirs to catch update changes
    file->type = RLFS_TYPE_REG;
    rlfs_mlist_append(lfs, (struct rlfs_mlist *)file);

#ifdef RLFS_READONLY
    if (tag == RLFS_ERR_NOENT) {
        err = RLFS_ERR_NOENT;
        goto cleanup;
#else
    if (tag == RLFS_ERR_NOENT) {
        if (!(flags & RLFS_O_CREAT)) {
            err = RLFS_ERR_NOENT;
            goto cleanup;
        }

        // don't allow trailing slashes
        if (rlfs_path_isdir(path)) {
            err = RLFS_ERR_NOTDIR;
            goto cleanup;
        }

        // check that name fits
        rlfs_size_t nlen = rlfs_path_namelen(path);
        if (nlen > lfs->name_max) {
            err = RLFS_ERR_NAMETOOLONG;
            goto cleanup;
        }

        // get next slot and create entry to remember name
        err = rlfs_dir_commit(lfs, &file->m, RLFS_MKATTRS(
                {RLFS_MKTAG(RLFS_TYPE_CREATE, file->id, 0), NULL},
                {RLFS_MKTAG(RLFS_TYPE_REG, file->id, nlen), path},
                {RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, file->id, 0), NULL}));

        // it may happen that the file name doesn't fit in the metadata blocks, e.g., a 256 byte file name will
        // not fit in a 128 byte block.
        err = (err == RLFS_ERR_NOSPC) ? RLFS_ERR_NAMETOOLONG : err;
        if (err) {
            goto cleanup;
        }

        tag = RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, 0);
    } else if (flags & RLFS_O_EXCL) {
        err = RLFS_ERR_EXIST;
        goto cleanup;
#endif
    } else if (rlfs_tag_type3(tag) != RLFS_TYPE_REG) {
        err = RLFS_ERR_ISDIR;
        goto cleanup;
#ifndef RLFS_READONLY
    } else if (flags & RLFS_O_TRUNC) {
        // truncate if requested
        tag = RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, file->id, 0);
        file->flags |= RLFS_F_DIRTY;
#endif
    } else {
        // try to load what's on disk, if it's inlined we'll fix it later
        tag = rlfs_dir_get(lfs, &file->m, RLFS_MKTAG(0x700, 0x3ff, 0),
                RLFS_MKTAG(RLFS_TYPE_STRUCT, file->id, 8), &file->ctz);
        if (tag < 0) {
            err = tag;
            goto cleanup;
        }
        rlfs_ctz_fromle32(&file->ctz);
    }

    // fetch attrs
    for (unsigned i = 0; i < file->cfg->attr_count; i++) {
        // if opened for read / read-write operations
        if ((file->flags & RLFS_O_RDONLY) == RLFS_O_RDONLY) {
            rlfs_stag_t res = rlfs_dir_get(lfs, &file->m,
                    RLFS_MKTAG(0x7ff, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_USERATTR + file->cfg->attrs[i].type,
                        file->id, file->cfg->attrs[i].size),
                        file->cfg->attrs[i].buffer);
            if (res < 0 && res != RLFS_ERR_NOENT) {
                err = res;
                goto cleanup;
            }
        }

#ifndef RLFS_READONLY
        // if opened for write / read-write operations
        if ((file->flags & RLFS_O_WRONLY) == RLFS_O_WRONLY) {
            if (file->cfg->attrs[i].size > lfs->attr_max) {
                err = RLFS_ERR_NOSPC;
                goto cleanup;
            }

            file->flags |= RLFS_F_DIRTY;
        }
#endif
    }

    // allocate buffer if needed
    if (file->cfg->buffer) {
        file->cache.buffer = file->cfg->buffer;
    } else {
        file->cache.buffer = rlfs_malloc(lfs->cfg->cache_size);
        if (!file->cache.buffer) {
            err = RLFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // zero to avoid information leak
    rlfs_cache_zero(lfs, &file->cache);

    if (rlfs_tag_type3(tag) == RLFS_TYPE_INLINESTRUCT) {
        // load inline files
        file->ctz.head = RLFS_BLOCK_INLINE;
        file->ctz.size = rlfs_tag_size(tag);
        file->flags |= RLFS_F_INLINE;
        file->cache.block = file->ctz.head;
        file->cache.off = 0;
        file->cache.size = lfs->cfg->cache_size;

        // don't always read (may be new/trunc file)
        if (file->ctz.size > 0) {
            rlfs_stag_t res = rlfs_dir_get(lfs, &file->m,
                    RLFS_MKTAG(0x700, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_STRUCT, file->id,
                        rlfs_min(file->cache.size, 0x3fe)),
                    file->cache.buffer);
            if (res < 0) {
                err = res;
                goto cleanup;
            }
        }
    }

    return 0;

cleanup:
    // clean up lingering resources
#ifndef RLFS_READONLY
    file->flags |= RLFS_F_ERRED;
#endif
    rlfs_file_close_(lfs, file);
    return err;
}

#ifndef RLFS_NO_MALLOC
static int rlfs_file_open_(rlfs_t *lfs, rlfs_file_t *file,
        const char *path, int flags) {
    static const struct rlfs_file_config defaults = {0};
    int err = rlfs_file_opencfg_(lfs, file, path, flags, &defaults);
    return err;
}
#endif

static int rlfs_file_close_(rlfs_t *lfs, rlfs_file_t *file) {
#ifndef RLFS_READONLY
    int err = rlfs_file_sync_(lfs, file);
#else
    int err = 0;
#endif

    // remove from list of mdirs
    rlfs_mlist_remove(lfs, (struct rlfs_mlist*)file);

    // clean up memory
    if (!file->cfg->buffer) {
        rlfs_free(file->cache.buffer);
    }

    return err;
}


#ifndef RLFS_READONLY
static int rlfs_file_relocate(rlfs_t *lfs, rlfs_file_t *file) {
    while (true) {
        // just relocate what exists into new block
        rlfs_block_t nblock;
        int err = rlfs_alloc(lfs, &nblock);
        if (err) {
            return err;
        }

        err = rlfs_bd_erase(lfs, nblock);
        if (err) {
            if (err == RLFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // either read from dirty cache or disk
        for (rlfs_off_t i = 0; i < file->off; i++) {
            uint8_t data;
            if (file->flags & RLFS_F_INLINE) {
                err = rlfs_dir_getread(lfs, &file->m,
                        // note we evict inline files before they can be dirty
                        NULL, &file->cache, file->off-i,
                        RLFS_MKTAG(0xfff, 0x1ff, 0),
                        RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, file->id, 0),
                        i, &data, 1);
                if (err) {
                    return err;
                }
            } else {
                err = rlfs_bd_read(lfs,
                        &file->cache, &lfs->rcache, file->off-i,
                        file->block, i, &data, 1);
                if (err) {
                    return err;
                }
            }

            err = rlfs_bd_prog(lfs,
                    &lfs->pcache, &lfs->rcache, true,
                    nblock, i, &data, 1);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        // copy over new state of file
        memcpy(file->cache.buffer, lfs->pcache.buffer, lfs->cfg->cache_size);
        file->cache.block = lfs->pcache.block;
        file->cache.off = lfs->pcache.off;
        file->cache.size = lfs->pcache.size;
        rlfs_cache_zero(lfs, &lfs->pcache);

        file->block = nblock;
        file->flags |= RLFS_F_WRITING;
        return 0;

relocate:
        RLFS_DEBUG("Bad block at 0x%"PRIx32, nblock);

        // just clear cache and try a new block
        rlfs_cache_drop(lfs, &lfs->pcache);
    }
}
#endif

#ifndef RLFS_READONLY
static int rlfs_file_outline(rlfs_t *lfs, rlfs_file_t *file) {
    file->off = file->pos;
    rlfs_alloc_ckpoint(lfs);
    int err = rlfs_file_relocate(lfs, file);
    if (err) {
        return err;
    }

    file->flags &= ~RLFS_F_INLINE;
    return 0;
}
#endif

static int rlfs_file_flush(rlfs_t *lfs, rlfs_file_t *file) {
    if (file->flags & RLFS_F_READING) {
        if (!(file->flags & RLFS_F_INLINE)) {
            rlfs_cache_drop(lfs, &file->cache);
        }
        file->flags &= ~RLFS_F_READING;
    }

#ifndef RLFS_READONLY
    if (file->flags & RLFS_F_WRITING) {
        rlfs_off_t pos = file->pos;

        if (!(file->flags & RLFS_F_INLINE)) {
            // copy over anything after current branch
            rlfs_file_t orig = {
                .ctz.head = file->ctz.head,
                .ctz.size = file->ctz.size,
                .flags = RLFS_O_RDONLY,
                .pos = file->pos,
                .cache = lfs->rcache,
            };
            rlfs_cache_drop(lfs, &lfs->rcache);

            while (file->pos < file->ctz.size) {
                // copy over a byte at a time, leave it up to caching
                // to make this efficient
                uint8_t data;
                rlfs_ssize_t res = rlfs_file_flushedread(lfs, &orig, &data, 1);
                if (res < 0) {
                    return res;
                }

                res = rlfs_file_flushedwrite(lfs, file, &data, 1);
                if (res < 0) {
                    return res;
                }

                // keep our reference to the rcache in sync
                if (lfs->rcache.block != RLFS_BLOCK_NULL) {
                    rlfs_cache_drop(lfs, &orig.cache);
                    rlfs_cache_drop(lfs, &lfs->rcache);
                }
            }

            // write out what we have
            while (true) {
                int err = rlfs_bd_flush(lfs, &file->cache, &lfs->rcache, true);
                if (err) {
                    if (err == RLFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                break;

relocate:
                RLFS_DEBUG("Bad block at 0x%"PRIx32, file->block);
                err = rlfs_file_relocate(lfs, file);
                if (err) {
                    return err;
                }
            }
        } else {
            file->pos = rlfs_max(file->pos, file->ctz.size);
        }

        // actual file updates
        file->ctz.head = file->block;
        file->ctz.size = file->pos;
        file->flags &= ~RLFS_F_WRITING;
        file->flags |= RLFS_F_DIRTY;

        file->pos = pos;
    }
#endif

    return 0;
}

#ifndef RLFS_READONLY
static int rlfs_file_sync_(rlfs_t *lfs, rlfs_file_t *file) {
    if (file->flags & RLFS_F_ERRED) {
        // it's not safe to do anything if our file errored
        return 0;
    }

    int err = rlfs_file_flush(lfs, file);
    if (err) {
        file->flags |= RLFS_F_ERRED;
        return err;
    }


    if ((file->flags & RLFS_F_DIRTY) &&
            !rlfs_pair_isnull(file->m.pair)) {
        // before we commit metadata, we need sync the disk to make sure
        // data writes don't complete after metadata writes
        if (!(file->flags & RLFS_F_INLINE)) {
            err = rlfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
            if (err) {
                return err;
            }
        }

        // update dir entry
        uint16_t type;
        const void *buffer;
        rlfs_size_t size;
        struct rlfs_ctz ctz;
        if (file->flags & RLFS_F_INLINE) {
            // inline the whole file
            type = RLFS_TYPE_INLINESTRUCT;
            buffer = file->cache.buffer;
            size = file->ctz.size;
        } else {
            // update the ctz reference
            type = RLFS_TYPE_CTZSTRUCT;
            // copy ctz so alloc will work during a relocate
            ctz = file->ctz;
            rlfs_ctz_tole32(&ctz);
            buffer = &ctz;
            size = sizeof(ctz);
        }

        // commit file data and attributes
        err = rlfs_dir_commit(lfs, &file->m, RLFS_MKATTRS(
                {RLFS_MKTAG(type, file->id, size), buffer},
                {RLFS_MKTAG(RLFS_FROM_USERATTRS, file->id,
                    file->cfg->attr_count), file->cfg->attrs}));
        if (err) {
            file->flags |= RLFS_F_ERRED;
            return err;
        }

        file->flags &= ~RLFS_F_DIRTY;
    }

    return 0;
}
#endif

static rlfs_ssize_t rlfs_file_flushedread(rlfs_t *lfs, rlfs_file_t *file,
        void *buffer, rlfs_size_t size) {
    uint8_t *data = buffer;
    rlfs_size_t nsize = size;

    if (file->pos >= file->ctz.size) {
        // eof if past end
        return 0;
    }

    size = rlfs_min(size, file->ctz.size - file->pos);
    nsize = size;

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & RLFS_F_READING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & RLFS_F_INLINE)) {
                int err = rlfs_ctz_find(lfs, NULL, &file->cache,
                        file->ctz.head, file->ctz.size,
                        file->pos, &file->block, &file->off);
                if (err) {
                    return err;
                }
            } else {
                file->block = RLFS_BLOCK_INLINE;
                file->off = file->pos;
            }

            file->flags |= RLFS_F_READING;
        }

        // read as much as we can in current block
        rlfs_size_t diff = rlfs_min(nsize, lfs->cfg->block_size - file->off);
        if (file->flags & RLFS_F_INLINE) {
            int err = rlfs_dir_getread(lfs, &file->m,
                    NULL, &file->cache, lfs->cfg->block_size,
                    RLFS_MKTAG(0xfff, 0x1ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, file->id, 0),
                    file->off, data, diff);
            if (err) {
                return err;
            }
        } else {
            int err = rlfs_bd_read(lfs,
                    NULL, &file->cache, lfs->cfg->block_size,
                    file->block, file->off, data, diff);
            if (err) {
                return err;
            }
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;
    }

    return size;
}

static rlfs_ssize_t rlfs_file_read_(rlfs_t *lfs, rlfs_file_t *file,
        void *buffer, rlfs_size_t size) {
    RLFS_ASSERT((file->flags & RLFS_O_RDONLY) == RLFS_O_RDONLY);

#ifndef RLFS_READONLY
    if (file->flags & RLFS_F_WRITING) {
        // flush out any writes
        int err = rlfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }
#endif

    return rlfs_file_flushedread(lfs, file, buffer, size);
}


#ifndef RLFS_READONLY
static rlfs_ssize_t rlfs_file_flushedwrite(rlfs_t *lfs, rlfs_file_t *file,
        const void *buffer, rlfs_size_t size) {
    const uint8_t *data = buffer;
    rlfs_size_t nsize = size;

    if ((file->flags & RLFS_F_INLINE) &&
            rlfs_max(file->pos+nsize, file->ctz.size) > lfs->inline_max) {
        // inline file doesn't fit anymore
        int err = rlfs_file_outline(lfs, file);
        if (err) {
            file->flags |= RLFS_F_ERRED;
            return err;
        }
    }

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & RLFS_F_WRITING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & RLFS_F_INLINE)) {
                if (!(file->flags & RLFS_F_WRITING) && file->pos > 0) {
                    // find out which block we're extending from
                    int err = rlfs_ctz_find(lfs, NULL, &file->cache,
                            file->ctz.head, file->ctz.size,
                            file->pos-1, &file->block, &(rlfs_off_t){0});
                    if (err) {
                        file->flags |= RLFS_F_ERRED;
                        return err;
                    }

                    // mark cache as dirty since we may have read data into it
                    rlfs_cache_zero(lfs, &file->cache);
                }

                // extend file with new blocks
                rlfs_alloc_ckpoint(lfs);
                int err = rlfs_ctz_extend(lfs, &file->cache, &lfs->rcache,
                        file->block, file->pos,
                        &file->block, &file->off);
                if (err) {
                    file->flags |= RLFS_F_ERRED;
                    return err;
                }
            } else {
                file->block = RLFS_BLOCK_INLINE;
                file->off = file->pos;
            }

            file->flags |= RLFS_F_WRITING;
        }

        // program as much as we can in current block
        rlfs_size_t diff = rlfs_min(nsize, lfs->cfg->block_size - file->off);
        while (true) {
            int err = rlfs_bd_prog(lfs, &file->cache, &lfs->rcache, true,
                    file->block, file->off, data, diff);
            if (err) {
                if (err == RLFS_ERR_CORRUPT) {
                    goto relocate;
                }
                file->flags |= RLFS_F_ERRED;
                return err;
            }

            break;
relocate:
            err = rlfs_file_relocate(lfs, file);
            if (err) {
                file->flags |= RLFS_F_ERRED;
                return err;
            }
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;

        rlfs_alloc_ckpoint(lfs);
    }

    return size;
}

static rlfs_ssize_t rlfs_file_write_(rlfs_t *lfs, rlfs_file_t *file,
        const void *buffer, rlfs_size_t size) {
    RLFS_ASSERT((file->flags & RLFS_O_WRONLY) == RLFS_O_WRONLY);

    if (file->flags & RLFS_F_READING) {
        // drop any reads
        int err = rlfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    if ((file->flags & RLFS_O_APPEND) && file->pos < file->ctz.size) {
        file->pos = file->ctz.size;
    }

    if (file->pos + size > lfs->file_max) {
        // Larger than file limit?
        return RLFS_ERR_FBIG;
    }

    if (!(file->flags & RLFS_F_WRITING) && file->pos > file->ctz.size) {
        // fill with zeros
        rlfs_off_t pos = file->pos;
        file->pos = file->ctz.size;

        while (file->pos < pos) {
            rlfs_ssize_t res = rlfs_file_flushedwrite(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return res;
            }
        }
    }

    rlfs_ssize_t nsize = rlfs_file_flushedwrite(lfs, file, buffer, size);
    if (nsize < 0) {
        return nsize;
    }

    file->flags &= ~RLFS_F_ERRED;
    return nsize;
}
#endif

static rlfs_soff_t rlfs_file_seek_(rlfs_t *lfs, rlfs_file_t *file,
        rlfs_soff_t off, int whence) {
    // find new pos
    //
    // fortunately for us, littlefs is limited to 31-bit file sizes, so we
    // don't have to worry too much about integer overflow
    rlfs_off_t npos = file->pos;
    if (whence == RLFS_SEEK_SET) {
        npos = off;
    } else if (whence == RLFS_SEEK_CUR) {
        npos = file->pos + (rlfs_off_t)off;
    } else if (whence == RLFS_SEEK_END) {
        npos = (rlfs_off_t)rlfs_file_size_(lfs, file) + (rlfs_off_t)off;
    }

    if (npos > lfs->file_max) {
        // file position out of range
        return RLFS_ERR_INVAL;
    }

    if (file->pos == npos) {
        // noop - position has not changed
        return npos;
    }

    // if we're only reading and our new offset is still in the file's cache
    // we can avoid flushing and needing to reread the data
    if ((file->flags & RLFS_F_READING)
            && file->off != lfs->cfg->block_size) {
        int oindex = rlfs_ctz_index(lfs, &(rlfs_off_t){file->pos});
        rlfs_off_t noff = npos;
        int nindex = rlfs_ctz_index(lfs, &noff);
        if (oindex == nindex
                && noff >= file->cache.off
                && noff < file->cache.off + file->cache.size) {
            file->pos = npos;
            file->off = noff;
            return npos;
        }
    }

    // write out everything beforehand, may be noop if rdonly
    int err = rlfs_file_flush(lfs, file);
    if (err) {
        return err;
    }

    // update pos
    file->pos = npos;
    return npos;
}

#ifndef RLFS_READONLY
static int rlfs_file_truncate_(rlfs_t *lfs, rlfs_file_t *file, rlfs_off_t size) {
    RLFS_ASSERT((file->flags & RLFS_O_WRONLY) == RLFS_O_WRONLY);

    if (size > RLFS_FILE_MAX) {
        return RLFS_ERR_INVAL;
    }

    rlfs_off_t pos = file->pos;
    rlfs_off_t oldsize = rlfs_file_size_(lfs, file);
    if (size < oldsize) {
        // revert to inline file?
        if (size <= lfs->inline_max) {
            // flush+seek to head
            rlfs_soff_t res = rlfs_file_seek_(lfs, file, 0, RLFS_SEEK_SET);
            if (res < 0) {
                return (int)res;
            }

            // read our data into rcache temporarily
            rlfs_cache_drop(lfs, &lfs->rcache);
            res = rlfs_file_flushedread(lfs, file,
                    lfs->rcache.buffer, size);
            if (res < 0) {
                return (int)res;
            }

            file->ctz.head = RLFS_BLOCK_INLINE;
            file->ctz.size = size;
            file->flags |= RLFS_F_DIRTY | RLFS_F_READING | RLFS_F_INLINE;
            file->cache.block = file->ctz.head;
            file->cache.off = 0;
            file->cache.size = lfs->cfg->cache_size;
            memcpy(file->cache.buffer, lfs->rcache.buffer, size);

        } else {
            // need to flush since directly changing metadata
            int err = rlfs_file_flush(lfs, file);
            if (err) {
                return err;
            }

            // lookup new head in ctz skip list
            err = rlfs_ctz_find(lfs, NULL, &file->cache,
                    file->ctz.head, file->ctz.size,
                    size-1, &file->block, &(rlfs_off_t){0});
            if (err) {
                return err;
            }

            // need to set pos/block/off consistently so seeking back to
            // the old position does not get confused
            file->pos = size;
            file->ctz.head = file->block;
            file->ctz.size = size;
            file->flags |= RLFS_F_DIRTY | RLFS_F_READING;
        }
    } else if (size > oldsize) {
        // flush+seek if not already at end
        rlfs_soff_t res = rlfs_file_seek_(lfs, file, 0, RLFS_SEEK_END);
        if (res < 0) {
            return (int)res;
        }

        // fill with zeros
        while (file->pos < size) {
            res = rlfs_file_write_(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return (int)res;
            }
        }
    }

    // restore pos
    rlfs_soff_t res = rlfs_file_seek_(lfs, file, pos, RLFS_SEEK_SET);
    if (res < 0) {
      return (int)res;
    }

    return 0;
}
#endif

static rlfs_soff_t rlfs_file_tell_(rlfs_t *lfs, rlfs_file_t *file) {
    (void)lfs;
    return file->pos;
}

static int rlfs_file_rewind_(rlfs_t *lfs, rlfs_file_t *file) {
    rlfs_soff_t res = rlfs_file_seek_(lfs, file, 0, RLFS_SEEK_SET);
    if (res < 0) {
        return (int)res;
    }

    return 0;
}

static rlfs_soff_t rlfs_file_size_(rlfs_t *lfs, rlfs_file_t *file) {
    (void)lfs;

#ifndef RLFS_READONLY
    if (file->flags & RLFS_F_WRITING) {
        return rlfs_max(file->pos, file->ctz.size);
    }
#endif

    return file->ctz.size;
}


/// General fs operations ///
static int rlfs_stat_(rlfs_t *lfs, const char *path, struct rlfs_info *info) {
    rlfs_mdir_t cwd;
    rlfs_stag_t tag = rlfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0) {
        return (int)tag;
    }

    // only allow trailing slashes on dirs
    if (strchr(path, '/') != NULL
            && rlfs_tag_type3(tag) != RLFS_TYPE_DIR) {
        return RLFS_ERR_NOTDIR;
    }

    return rlfs_dir_getinfo(lfs, &cwd, rlfs_tag_id(tag), info);
}

#ifndef RLFS_READONLY
static int rlfs_remove_(rlfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = rlfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    rlfs_mdir_t cwd;
    rlfs_stag_t tag = rlfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0 || rlfs_tag_id(tag) == 0x3ff) {
        return (tag < 0) ? (int)tag : RLFS_ERR_INVAL;
    }

    struct rlfs_mlist dir;
    dir.next = lfs->mlist;
    if (rlfs_tag_type3(tag) == RLFS_TYPE_DIR) {
        // must be empty before removal
        rlfs_block_t pair[2];
        rlfs_stag_t res = rlfs_dir_get(lfs, &cwd, RLFS_MKTAG(0x700, 0x3ff, 0),
                RLFS_MKTAG(RLFS_TYPE_STRUCT, rlfs_tag_id(tag), 8), pair);
        if (res < 0) {
            return (int)res;
        }
        rlfs_pair_fromle32(pair);

        err = rlfs_dir_fetch(lfs, &dir.m, pair);
        if (err) {
            return err;
        }

        if (dir.m.count > 0 || dir.m.split) {
            return RLFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        err = rlfs_fs_preporphans(lfs, +1);
        if (err) {
            return err;
        }

        // I know it's crazy but yes, dir can be changed by our parent's
        // commit (if predecessor is child)
        dir.type = 0;
        dir.id = 0;
        lfs->mlist = &dir;
    }

    // delete the entry
    err = rlfs_dir_commit(lfs, &cwd, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_DELETE, rlfs_tag_id(tag), 0), NULL}));
    if (err) {
        lfs->mlist = dir.next;
        return err;
    }

    lfs->mlist = dir.next;
    if (rlfs_gstate_hasorphans(&lfs->gstate)) {
        RLFS_ASSERT(rlfs_tag_type3(tag) == RLFS_TYPE_DIR);

        // fix orphan
        err = rlfs_fs_preporphans(lfs, -1);
        if (err) {
            return err;
        }

        err = rlfs_fs_pred(lfs, dir.m.pair, &cwd);
        if (err) {
            return err;
        }

        err = rlfs_dir_drop(lfs, &cwd, &dir.m);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_rename_(rlfs_t *lfs, const char *oldpath, const char *newpath) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = rlfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // find old entry
    rlfs_mdir_t oldcwd;
    rlfs_stag_t oldtag = rlfs_dir_find(lfs, &oldcwd, &oldpath, NULL);
    if (oldtag < 0 || rlfs_tag_id(oldtag) == 0x3ff) {
        return (oldtag < 0) ? (int)oldtag : RLFS_ERR_INVAL;
    }

    // find new entry
    rlfs_mdir_t newcwd;
    uint16_t newid;
    rlfs_stag_t prevtag = rlfs_dir_find(lfs, &newcwd, &newpath, &newid);
    if ((prevtag < 0 || rlfs_tag_id(prevtag) == 0x3ff) &&
            !(prevtag == RLFS_ERR_NOENT && rlfs_path_islast(newpath))) {
        return (prevtag < 0) ? (int)prevtag : RLFS_ERR_INVAL;
    }

    // if we're in the same pair there's a few special cases...
    bool samepair = (rlfs_pair_cmp(oldcwd.pair, newcwd.pair) == 0);
    uint16_t newoldid = rlfs_tag_id(oldtag);

    struct rlfs_mlist prevdir;
    prevdir.next = lfs->mlist;
    if (prevtag == RLFS_ERR_NOENT) {
        // if we're a file, don't allow trailing slashes
        if (rlfs_path_isdir(newpath)
                && rlfs_tag_type3(oldtag) != RLFS_TYPE_DIR) {
            return RLFS_ERR_NOTDIR;
        }

        // check that name fits
        rlfs_size_t nlen = rlfs_path_namelen(newpath);
        if (nlen > lfs->name_max) {
            return RLFS_ERR_NAMETOOLONG;
        }

        // there is a small chance we are being renamed in the same
        // directory/ to an id less than our old id, the global update
        // to handle this is a bit messy
        if (samepair && newid <= newoldid) {
            newoldid += 1;
        }
    } else if (rlfs_tag_type3(prevtag) != rlfs_tag_type3(oldtag)) {
        return (rlfs_tag_type3(prevtag) == RLFS_TYPE_DIR)
                ? RLFS_ERR_ISDIR
                : RLFS_ERR_NOTDIR;
    } else if (samepair && newid == newoldid) {
        // we're renaming to ourselves??
        return 0;
    } else if (rlfs_tag_type3(prevtag) == RLFS_TYPE_DIR) {
        // must be empty before removal
        rlfs_block_t prevpair[2];
        rlfs_stag_t res = rlfs_dir_get(lfs, &newcwd, RLFS_MKTAG(0x700, 0x3ff, 0),
                RLFS_MKTAG(RLFS_TYPE_STRUCT, newid, 8), prevpair);
        if (res < 0) {
            return (int)res;
        }
        rlfs_pair_fromle32(prevpair);

        // must be empty before removal
        err = rlfs_dir_fetch(lfs, &prevdir.m, prevpair);
        if (err) {
            return err;
        }

        if (prevdir.m.count > 0 || prevdir.m.split) {
            return RLFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        err = rlfs_fs_preporphans(lfs, +1);
        if (err) {
            return err;
        }

        // I know it's crazy but yes, dir can be changed by our parent's
        // commit (if predecessor is child)
        prevdir.type = 0;
        prevdir.id = 0;
        lfs->mlist = &prevdir;
    }

    if (!samepair) {
        rlfs_fs_prepmove(lfs, newoldid, oldcwd.pair);
    }

    // move over all attributes
    err = rlfs_dir_commit(lfs, &newcwd, RLFS_MKATTRS(
            {RLFS_MKTAG_IF(prevtag != RLFS_ERR_NOENT,
                RLFS_TYPE_DELETE, newid, 0), NULL},
            {RLFS_MKTAG(RLFS_TYPE_CREATE, newid, 0), NULL},
            {RLFS_MKTAG(rlfs_tag_type3(oldtag),
                newid, rlfs_path_namelen(newpath)), newpath},
            {RLFS_MKTAG(RLFS_FROM_MOVE, newid, rlfs_tag_id(oldtag)), &oldcwd},
            {RLFS_MKTAG_IF(samepair,
                RLFS_TYPE_DELETE, newoldid, 0), NULL}));
    if (err) {
        lfs->mlist = prevdir.next;
        return err;
    }

    // let commit clean up after move (if we're different! otherwise move
    // logic already fixed it for us)
    if (!samepair && rlfs_gstate_hasmove(&lfs->gstate)) {
        // prep gstate and delete move id
        rlfs_fs_prepmove(lfs, 0x3ff, NULL);
        err = rlfs_dir_commit(lfs, &oldcwd, RLFS_MKATTRS(
                {RLFS_MKTAG(RLFS_TYPE_DELETE, rlfs_tag_id(oldtag), 0), NULL}));
        if (err) {
            lfs->mlist = prevdir.next;
            return err;
        }
    }

    lfs->mlist = prevdir.next;
    if (rlfs_gstate_hasorphans(&lfs->gstate)) {
        RLFS_ASSERT(prevtag != RLFS_ERR_NOENT
                && rlfs_tag_type3(prevtag) == RLFS_TYPE_DIR);

        // fix orphan
        err = rlfs_fs_preporphans(lfs, -1);
        if (err) {
            return err;
        }

        err = rlfs_fs_pred(lfs, prevdir.m.pair, &newcwd);
        if (err) {
            return err;
        }

        err = rlfs_dir_drop(lfs, &newcwd, &prevdir.m);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

static rlfs_ssize_t rlfs_getattr_(rlfs_t *lfs, const char *path,
        uint8_t type, void *buffer, rlfs_size_t size) {
    rlfs_mdir_t cwd;
    rlfs_stag_t tag = rlfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0) {
        return tag;
    }

    uint16_t id = rlfs_tag_id(tag);
    if (id == 0x3ff) {
        // special case for root
        id = 0;
        int err = rlfs_dir_fetch(lfs, &cwd, lfs->root);
        if (err) {
            return err;
        }
    }

    tag = rlfs_dir_get(lfs, &cwd, RLFS_MKTAG(0x7ff, 0x3ff, 0),
            RLFS_MKTAG(RLFS_TYPE_USERATTR + type,
                id, rlfs_min(size, lfs->attr_max)),
            buffer);
    if (tag < 0) {
        if (tag == RLFS_ERR_NOENT) {
            return RLFS_ERR_NOATTR;
        }

        return tag;
    }

    return rlfs_tag_size(tag);
}

#ifndef RLFS_READONLY
static int rlfs_commitattr(rlfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, rlfs_size_t size) {
    rlfs_mdir_t cwd;
    rlfs_stag_t tag = rlfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0) {
        return tag;
    }

    uint16_t id = rlfs_tag_id(tag);
    if (id == 0x3ff) {
        // special case for root
        id = 0;
        int err = rlfs_dir_fetch(lfs, &cwd, lfs->root);
        if (err) {
            return err;
        }
    }

    return rlfs_dir_commit(lfs, &cwd, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_USERATTR + type, id, size), buffer}));
}
#endif

#ifndef RLFS_READONLY
static int rlfs_setattr_(rlfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, rlfs_size_t size) {
    if (size > lfs->attr_max) {
        return RLFS_ERR_NOSPC;
    }

    return rlfs_commitattr(lfs, path, type, buffer, size);
}
#endif

#ifndef RLFS_READONLY
static int rlfs_removeattr_(rlfs_t *lfs, const char *path, uint8_t type) {
    return rlfs_commitattr(lfs, path, type, NULL, 0x3ff);
}
#endif


/// Filesystem operations ///

// compile time checks, see lfs.h for why these limits exist
#if RLFS_NAME_MAX > 1022
#error "Invalid RLFS_NAME_MAX, must be <= 1022"
#endif

#if RLFS_FILE_MAX > 2147483647
#error "Invalid RLFS_FILE_MAX, must be <= 2147483647"
#endif

#if RLFS_ATTR_MAX > 1022
#error "Invalid RLFS_ATTR_MAX, must be <= 1022"
#endif

// common filesystem initialization
static int rlfs_init(rlfs_t *lfs, const struct rlfs_config *cfg) {
    lfs->cfg = cfg;
    lfs->block_count = cfg->block_count;  // May be 0
    int err = 0;

#ifdef RLFS_MULTIVERSION
    // this driver only supports minor version < current minor version
    RLFS_ASSERT(!lfs->cfg->disk_version || (
            (0xffff & (lfs->cfg->disk_version >> 16))
                    == RLFS_DISK_VERSION_MAJOR
                && (0xffff & (lfs->cfg->disk_version >> 0))
                    <= RLFS_DISK_VERSION_MINOR));
#endif

    // check that bool is a truthy-preserving type
    //
    // note the most common reason for this failure is a before-c99 compiler,
    // which littlefs currently does not support
    RLFS_ASSERT((bool)0x80000000);

    // check that the required io functions are provided
    RLFS_ASSERT(lfs->cfg->read != NULL);
#ifndef RLFS_READONLY
    RLFS_ASSERT(lfs->cfg->prog != NULL);
    RLFS_ASSERT(lfs->cfg->erase != NULL);
    RLFS_ASSERT(lfs->cfg->sync != NULL);
#endif

    // validate that the lfs-cfg sizes were initiated properly before
    // performing any arithmetic logics with them
    RLFS_ASSERT(lfs->cfg->read_size != 0);
    RLFS_ASSERT(lfs->cfg->prog_size != 0);
    RLFS_ASSERT(lfs->cfg->cache_size != 0);

    // check that block size is a multiple of cache size is a multiple
    // of prog and read sizes
    RLFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->read_size == 0);
    RLFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->prog_size == 0);
    RLFS_ASSERT(lfs->cfg->block_size % lfs->cfg->cache_size == 0);

    // check that the block size is large enough to fit all ctz pointers
    RLFS_ASSERT(lfs->cfg->block_size >= 128);
    // this is the exact calculation for all ctz pointers, if this fails
    // and the simpler assert above does not, math must be broken
    RLFS_ASSERT(4*rlfs_npw2(0xffffffff / (lfs->cfg->block_size-2*4))
            <= lfs->cfg->block_size);

    // block_cycles = 0 is no longer supported.
    //
    // block_cycles is the number of erase cycles before littlefs evicts
    // metadata logs as a part of wear leveling. Suggested values are in the
    // range of 100-1000, or set block_cycles to -1 to disable block-level
    // wear-leveling.
    RLFS_ASSERT(lfs->cfg->block_cycles != 0);

    // check that compact_thresh makes sense
    //
    // metadata can't be compacted below block_size/2, and metadata can't
    // exceed a block_size
    RLFS_ASSERT(lfs->cfg->compact_thresh == 0
            || lfs->cfg->compact_thresh >= lfs->cfg->block_size/2);
    RLFS_ASSERT(lfs->cfg->compact_thresh == (rlfs_size_t)-1
            || lfs->cfg->compact_thresh <= lfs->cfg->block_size);

    // check that metadata_max is a multiple of read_size and prog_size,
    // and a factor of the block_size
    RLFS_ASSERT(!lfs->cfg->metadata_max
            || lfs->cfg->metadata_max % lfs->cfg->read_size == 0);
    RLFS_ASSERT(!lfs->cfg->metadata_max
            || lfs->cfg->metadata_max % lfs->cfg->prog_size == 0);
    RLFS_ASSERT(!lfs->cfg->metadata_max
            || lfs->cfg->block_size % lfs->cfg->metadata_max == 0);

    // setup read cache
    if (lfs->cfg->read_buffer) {
        lfs->rcache.buffer = lfs->cfg->read_buffer;
    } else {
        lfs->rcache.buffer = rlfs_malloc(lfs->cfg->cache_size);
        if (!lfs->rcache.buffer) {
            err = RLFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // setup program cache
    if (lfs->cfg->prog_buffer) {
        lfs->pcache.buffer = lfs->cfg->prog_buffer;
    } else {
        lfs->pcache.buffer = rlfs_malloc(lfs->cfg->cache_size);
        if (!lfs->pcache.buffer) {
            err = RLFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // zero to avoid information leaks
    rlfs_cache_zero(lfs, &lfs->rcache);
    rlfs_cache_zero(lfs, &lfs->pcache);

    // setup lookahead buffer, note mount finishes initializing this after
    // we establish a decent pseudo-random seed
    RLFS_ASSERT(lfs->cfg->lookahead_size > 0);
    if (lfs->cfg->lookahead_buffer) {
        lfs->lookahead.buffer = lfs->cfg->lookahead_buffer;
    } else {
        lfs->lookahead.buffer = rlfs_malloc(lfs->cfg->lookahead_size);
        if (!lfs->lookahead.buffer) {
            err = RLFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // check that the size limits are sane
    RLFS_ASSERT(lfs->cfg->name_max <= RLFS_NAME_MAX);
    lfs->name_max = lfs->cfg->name_max;
    if (!lfs->name_max) {
        lfs->name_max = RLFS_NAME_MAX;
    }

    RLFS_ASSERT(lfs->cfg->file_max <= RLFS_FILE_MAX);
    lfs->file_max = lfs->cfg->file_max;
    if (!lfs->file_max) {
        lfs->file_max = RLFS_FILE_MAX;
    }

    RLFS_ASSERT(lfs->cfg->attr_max <= RLFS_ATTR_MAX);
    lfs->attr_max = lfs->cfg->attr_max;
    if (!lfs->attr_max) {
        lfs->attr_max = RLFS_ATTR_MAX;
    }

    RLFS_ASSERT(lfs->cfg->metadata_max <= lfs->cfg->block_size);

    RLFS_ASSERT(lfs->cfg->inline_max == (rlfs_size_t)-1
            || lfs->cfg->inline_max <= lfs->cfg->cache_size);
    RLFS_ASSERT(lfs->cfg->inline_max == (rlfs_size_t)-1
            || lfs->cfg->inline_max <= lfs->attr_max);
    RLFS_ASSERT(lfs->cfg->inline_max == (rlfs_size_t)-1
            || lfs->cfg->inline_max <= ((lfs->cfg->metadata_max)
                ? lfs->cfg->metadata_max
                : lfs->cfg->block_size)/8);
    lfs->inline_max = lfs->cfg->inline_max;
    if (lfs->inline_max == (rlfs_size_t)-1) {
        lfs->inline_max = 0;
    } else if (lfs->inline_max == 0) {
        lfs->inline_max = rlfs_min(
                lfs->cfg->cache_size,
                rlfs_min(
                    lfs->attr_max,
                    ((lfs->cfg->metadata_max)
                        ? lfs->cfg->metadata_max
                        : lfs->cfg->block_size)/8));
    }

    // setup default state
    lfs->root[0] = RLFS_BLOCK_NULL;
    lfs->root[1] = RLFS_BLOCK_NULL;
    lfs->mlist = NULL;
    lfs->seed = 0;
    lfs->gdisk = (rlfs_gstate_t){0};
    lfs->gstate = (rlfs_gstate_t){0};
    lfs->gdelta = (rlfs_gstate_t){0};
#ifdef RLFS_MIGRATE
    lfs->lfs1 = NULL;
#endif

    return 0;

cleanup:
    rlfs_deinit(lfs);
    return err;
}

static int rlfs_deinit(rlfs_t *lfs) {
    // free allocated memory
    if (!lfs->cfg->read_buffer) {
        rlfs_free(lfs->rcache.buffer);
    }

    if (!lfs->cfg->prog_buffer) {
        rlfs_free(lfs->pcache.buffer);
    }

    if (!lfs->cfg->lookahead_buffer) {
        rlfs_free(lfs->lookahead.buffer);
    }

    return 0;
}



#ifndef RLFS_READONLY
static int rlfs_format_(rlfs_t *lfs, const struct rlfs_config *cfg) {
    int err = 0;
    {
        err = rlfs_init(lfs, cfg);
        if (err) {
            return err;
        }

        RLFS_ASSERT(cfg->block_count != 0);

        // create free lookahead
        memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);
        lfs->lookahead.start = 0;
        lfs->lookahead.size = rlfs_min(8*lfs->cfg->lookahead_size,
                lfs->block_count);
        lfs->lookahead.next = 0;
        rlfs_alloc_ckpoint(lfs);

        // create root dir
        rlfs_mdir_t root;
        err = rlfs_dir_alloc(lfs, &root);
        if (err) {
            goto cleanup;
        }

        // write one superblock
        rlfs_superblock_t superblock = {
            .version     = rlfs_fs_disk_version(lfs),
            .block_size  = lfs->cfg->block_size,
            .block_count = lfs->block_count,
            .name_max    = lfs->name_max,
            .file_max    = lfs->file_max,
            .attr_max    = lfs->attr_max,
        };

        rlfs_superblock_tole32(&superblock);
        err = rlfs_dir_commit(lfs, &root, RLFS_MKATTRS(
                {RLFS_MKTAG(RLFS_TYPE_CREATE, 0, 0), NULL},
                {RLFS_MKTAG(RLFS_TYPE_SUPERBLOCK, 0, 8), "littlefs"},
                {RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                    &superblock}));
        if (err) {
            goto cleanup;
        }

        // force compaction to prevent accidentally mounting any
        // older version of littlefs that may live on disk
        root.erased = false;
        err = rlfs_dir_commit(lfs, &root, NULL, 0);
        if (err) {
            goto cleanup;
        }

        // sanity check that fetch works
        err = rlfs_dir_fetch(lfs, &root, (const rlfs_block_t[2]){0, 1});
        if (err) {
            goto cleanup;
        }
    }

cleanup:
    rlfs_deinit(lfs);
    return err;

}
#endif

struct rlfs_tortoise_t {
    rlfs_block_t pair[2];
    rlfs_size_t i;
    rlfs_size_t period;
};

static int rlfs_tortoise_detectcycles(
    const rlfs_mdir_t *dir, struct rlfs_tortoise_t *tortoise) {
    // detect cycles with Brent's algorithm
    if (rlfs_pair_issync(dir->tail, tortoise->pair)) {
        RLFS_WARN("Cycle detected in tail list");
        return RLFS_ERR_CORRUPT;
    }
    if (tortoise->i == tortoise->period) {
        tortoise->pair[0] = dir->tail[0];
        tortoise->pair[1] = dir->tail[1];
        tortoise->i = 0;
        tortoise->period *= 2;
    }
    tortoise->i += 1;

    return RLFS_ERR_OK;
}

static int rlfs_mount_(rlfs_t *lfs, const struct rlfs_config *cfg) {
    int err = rlfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // scan directory blocks for superblock and any global updates
    rlfs_mdir_t dir = {.tail = {0, 1}};
    struct rlfs_tortoise_t tortoise = {
        .pair = {RLFS_BLOCK_NULL, RLFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    while (!rlfs_pair_isnull(dir.tail)) {
        err = rlfs_tortoise_detectcycles(&dir, &tortoise);
        if (err < 0) {
            goto cleanup;
        }

        // fetch next block in tail list
        rlfs_stag_t tag = rlfs_dir_fetchmatch(lfs, &dir, dir.tail,
                RLFS_MKTAG(0x7ff, 0x3ff, 0),
                RLFS_MKTAG(RLFS_TYPE_SUPERBLOCK, 0, 8),
                NULL,
                rlfs_dir_find_match, &(struct rlfs_dir_find_match){
                    lfs, "littlefs", 8});
        if (tag < 0) {
            err = tag;
            goto cleanup;
        }

        // has superblock?
        if (tag && !rlfs_tag_isdelete(tag)) {
            // update root
            lfs->root[0] = dir.pair[0];
            lfs->root[1] = dir.pair[1];

            // grab superblock
            rlfs_superblock_t superblock;
            tag = rlfs_dir_get(lfs, &dir, RLFS_MKTAG(0x7ff, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                    &superblock);
            if (tag < 0) {
                err = tag;
                goto cleanup;
            }
            rlfs_superblock_fromle32(&superblock);

            // check version
            uint16_t major_version = (0xffff & (superblock.version >> 16));
            uint16_t minor_version = (0xffff & (superblock.version >>  0));
            if (major_version != rlfs_fs_disk_version_major(lfs)
                    || minor_version > rlfs_fs_disk_version_minor(lfs)) {
                RLFS_ERROR("Invalid version "
                        "v%"PRIu16".%"PRIu16" != v%"PRIu16".%"PRIu16,
                        major_version,
                        minor_version,
                        rlfs_fs_disk_version_major(lfs),
                        rlfs_fs_disk_version_minor(lfs));
                err = RLFS_ERR_INVAL;
                goto cleanup;
            }

            // found older minor version? set an in-device only bit in the
            // gstate so we know we need to rewrite the superblock before
            // the first write
            bool needssuperblock = false;
            if (minor_version < rlfs_fs_disk_version_minor(lfs)) {
                RLFS_DEBUG("Found older minor version "
                        "v%"PRIu16".%"PRIu16" < v%"PRIu16".%"PRIu16,
                        major_version,
                        minor_version,
                        rlfs_fs_disk_version_major(lfs),
                        rlfs_fs_disk_version_minor(lfs));
                needssuperblock = true;
            }
            // note this bit is reserved on disk, so fetching more gstate
            // will not interfere here
            rlfs_fs_prepsuperblock(lfs, needssuperblock);

            // check superblock configuration
            if (superblock.name_max) {
                if (superblock.name_max > lfs->name_max) {
                    RLFS_ERROR("Unsupported name_max (%"PRIu32" > %"PRIu32")",
                            superblock.name_max, lfs->name_max);
                    err = RLFS_ERR_INVAL;
                    goto cleanup;
                }

                lfs->name_max = superblock.name_max;
            }

            if (superblock.file_max) {
                if (superblock.file_max > lfs->file_max) {
                    RLFS_ERROR("Unsupported file_max (%"PRIu32" > %"PRIu32")",
                            superblock.file_max, lfs->file_max);
                    err = RLFS_ERR_INVAL;
                    goto cleanup;
                }

                lfs->file_max = superblock.file_max;
            }

            if (superblock.attr_max) {
                if (superblock.attr_max > lfs->attr_max) {
                    RLFS_ERROR("Unsupported attr_max (%"PRIu32" > %"PRIu32")",
                            superblock.attr_max, lfs->attr_max);
                    err = RLFS_ERR_INVAL;
                    goto cleanup;
                }

                lfs->attr_max = superblock.attr_max;

                // we also need to update inline_max in case attr_max changed
                lfs->inline_max = rlfs_min(lfs->inline_max, lfs->attr_max);
            }

            // this is where we get the block_count from disk if block_count=0
            if (lfs->cfg->block_count
                    && superblock.block_count != lfs->cfg->block_count) {
                RLFS_ERROR("Invalid block count (%"PRIu32" != %"PRIu32")",
                        superblock.block_count, lfs->cfg->block_count);
                err = RLFS_ERR_INVAL;
                goto cleanup;
            }

            lfs->block_count = superblock.block_count;

            if (superblock.block_size != lfs->cfg->block_size) {
                RLFS_ERROR("Invalid block size (%"PRIu32" != %"PRIu32")",
                        superblock.block_size, lfs->cfg->block_size);
                err = RLFS_ERR_INVAL;
                goto cleanup;
            }
        }

        // has gstate?
        err = rlfs_dir_getgstate(lfs, &dir, &lfs->gstate);
        if (err) {
            goto cleanup;
        }
    }

    // update littlefs with gstate
    if (!rlfs_gstate_iszero(&lfs->gstate)) {
        RLFS_DEBUG("Found pending gstate 0x%08"PRIx32"%08"PRIx32"%08"PRIx32,
                lfs->gstate.tag,
                lfs->gstate.pair[0],
                lfs->gstate.pair[1]);
    }
    lfs->gstate.tag += !rlfs_tag_isvalid(lfs->gstate.tag);
    lfs->gdisk = lfs->gstate;

    // setup free lookahead, to distribute allocations uniformly across
    // boots, we start the allocator at a random location
    lfs->lookahead.start = lfs->seed % lfs->block_count;
    rlfs_alloc_drop(lfs);

    return 0;

cleanup:
    rlfs_unmount_(lfs);
    return err;
}

static int rlfs_unmount_(rlfs_t *lfs) {
    return rlfs_deinit(lfs);
}


/// Filesystem filesystem operations ///
static int rlfs_fs_stat_(rlfs_t *lfs, struct rlfs_fsinfo *fsinfo) {
    // if the superblock is up-to-date, we must be on the most recent
    // minor version of littlefs
    if (!rlfs_gstate_needssuperblock(&lfs->gstate)) {
        fsinfo->disk_version = rlfs_fs_disk_version(lfs);

    // otherwise we need to read the minor version on disk
    } else {
        // fetch the superblock
        rlfs_mdir_t dir;
        int err = rlfs_dir_fetch(lfs, &dir, lfs->root);
        if (err) {
            return err;
        }

        rlfs_superblock_t superblock;
        rlfs_stag_t tag = rlfs_dir_get(lfs, &dir, RLFS_MKTAG(0x7ff, 0x3ff, 0),
                RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                &superblock);
        if (tag < 0) {
            return tag;
        }
        rlfs_superblock_fromle32(&superblock);

        // read the on-disk version
        fsinfo->disk_version = superblock.version;
    }

    // filesystem geometry
    fsinfo->block_size = lfs->cfg->block_size;
    fsinfo->block_count = lfs->block_count;

    // other on-disk configuration, we cache all of these for internal use
    fsinfo->name_max = lfs->name_max;
    fsinfo->file_max = lfs->file_max;
    fsinfo->attr_max = lfs->attr_max;

    return 0;
}

int rlfs_fs_traverse_(rlfs_t *lfs,
        int (*cb)(void *data, rlfs_block_t block), void *data,
        bool includeorphans) {
    // iterate over metadata pairs
    rlfs_mdir_t dir = {.tail = {0, 1}};

#ifdef RLFS_MIGRATE
    // also consider v1 blocks during migration
    if (lfs->lfs1) {
        int err = lfs1_traverse(lfs, cb, data);
        if (err) {
            return err;
        }

        dir.tail[0] = lfs->root[0];
        dir.tail[1] = lfs->root[1];
    }
#endif

    struct rlfs_tortoise_t tortoise = {
        .pair = {RLFS_BLOCK_NULL, RLFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    int err = RLFS_ERR_OK;
    while (!rlfs_pair_isnull(dir.tail)) {
        err = rlfs_tortoise_detectcycles(&dir, &tortoise);
        if (err < 0) {
            return RLFS_ERR_CORRUPT;
        }

        for (int i = 0; i < 2; i++) {
            int err = cb(data, dir.tail[i]);
            if (err) {
                return err;
            }
        }

        // iterate through ids in directory
        int err = rlfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            return err;
        }

        for (uint16_t id = 0; id < dir.count; id++) {
            struct rlfs_ctz ctz;
            rlfs_stag_t tag = rlfs_dir_get(lfs, &dir, RLFS_MKTAG(0x700, 0x3ff, 0),
                    RLFS_MKTAG(RLFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
            if (tag < 0) {
                if (tag == RLFS_ERR_NOENT) {
                    continue;
                }
                return tag;
            }
            rlfs_ctz_fromle32(&ctz);

            if (rlfs_tag_type3(tag) == RLFS_TYPE_CTZSTRUCT) {
                err = rlfs_ctz_traverse(lfs, NULL, &lfs->rcache,
                        ctz.head, ctz.size, cb, data);
                if (err) {
                    return err;
                }
            } else if (includeorphans &&
                    rlfs_tag_type3(tag) == RLFS_TYPE_DIRSTRUCT) {
                for (int i = 0; i < 2; i++) {
                    err = cb(data, (&ctz.head)[i]);
                    if (err) {
                        return err;
                    }
                }
            }
        }
    }

#ifndef RLFS_READONLY
    // iterate over any open files
    for (rlfs_file_t *f = (rlfs_file_t*)lfs->mlist; f; f = f->next) {
        if (f->type != RLFS_TYPE_REG) {
            continue;
        }

        if ((f->flags & RLFS_F_DIRTY) && !(f->flags & RLFS_F_INLINE)) {
            int err = rlfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
                    f->ctz.head, f->ctz.size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & RLFS_F_WRITING) && !(f->flags & RLFS_F_INLINE)) {
            int err = rlfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
                    f->block, f->pos, cb, data);
            if (err) {
                return err;
            }
        }
    }
#endif

    return 0;
}

#ifndef RLFS_READONLY
static int rlfs_fs_pred(rlfs_t *lfs,
        const rlfs_block_t pair[2], rlfs_mdir_t *pdir) {
    // iterate over all directory directory entries
    pdir->tail[0] = 0;
    pdir->tail[1] = 1;
    struct rlfs_tortoise_t tortoise = {
        .pair = {RLFS_BLOCK_NULL, RLFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    int err = RLFS_ERR_OK;
    while (!rlfs_pair_isnull(pdir->tail)) {
        err = rlfs_tortoise_detectcycles(pdir, &tortoise);
        if (err < 0) {
            return RLFS_ERR_CORRUPT;
        }

        if (rlfs_pair_cmp(pdir->tail, pair) == 0) {
            return 0;
        }

        int err = rlfs_dir_fetch(lfs, pdir, pdir->tail);
        if (err) {
            return err;
        }
    }

    return RLFS_ERR_NOENT;
}
#endif

#ifndef RLFS_READONLY
struct rlfs_fs_parent_match {
    rlfs_t *lfs;
    const rlfs_block_t pair[2];
};
#endif

#ifndef RLFS_READONLY
static int rlfs_fs_parent_match(void *data,
        rlfs_tag_t tag, const void *buffer) {
    struct rlfs_fs_parent_match *find = data;
    rlfs_t *lfs = find->lfs;
    const struct rlfs_diskoff *disk = buffer;
    (void)tag;

    rlfs_block_t child[2];
    int err = rlfs_bd_read(lfs,
            &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
            disk->block, disk->off, &child, sizeof(child));
    if (err) {
        return err;
    }

    rlfs_pair_fromle32(child);
    return (rlfs_pair_cmp(child, find->pair) == 0) ? RLFS_CMP_EQ : RLFS_CMP_LT;
}
#endif

#ifndef RLFS_READONLY
static rlfs_stag_t rlfs_fs_parent(rlfs_t *lfs, const rlfs_block_t pair[2],
        rlfs_mdir_t *parent) {
    // use fetchmatch with callback to find pairs
    parent->tail[0] = 0;
    parent->tail[1] = 1;
    struct rlfs_tortoise_t tortoise = {
        .pair = {RLFS_BLOCK_NULL, RLFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    int err = RLFS_ERR_OK;
    while (!rlfs_pair_isnull(parent->tail)) {
        err = rlfs_tortoise_detectcycles(parent, &tortoise);
        if (err < 0) {
            return err;
        }

        rlfs_stag_t tag = rlfs_dir_fetchmatch(lfs, parent, parent->tail,
                RLFS_MKTAG(0x7ff, 0, 0x3ff),
                RLFS_MKTAG(RLFS_TYPE_DIRSTRUCT, 0, 8),
                NULL,
                rlfs_fs_parent_match, &(struct rlfs_fs_parent_match){
                    lfs, {pair[0], pair[1]}});
        if (tag && tag != RLFS_ERR_NOENT) {
            return tag;
        }
    }

    return RLFS_ERR_NOENT;
}
#endif

static void rlfs_fs_prepsuperblock(rlfs_t *lfs, bool needssuperblock) {
    lfs->gstate.tag = (lfs->gstate.tag & ~RLFS_MKTAG(0, 0, 0x200))
            | (uint32_t)needssuperblock << 9;
}

#ifndef RLFS_READONLY
static int rlfs_fs_preporphans(rlfs_t *lfs, int8_t orphans) {
    RLFS_ASSERT(rlfs_tag_size(lfs->gstate.tag) > 0x000 || orphans >= 0);
    RLFS_ASSERT(rlfs_tag_size(lfs->gstate.tag) < 0x1ff || orphans <= 0);
    lfs->gstate.tag += orphans;
    lfs->gstate.tag = ((lfs->gstate.tag & ~RLFS_MKTAG(0x800, 0, 0)) |
            ((uint32_t)rlfs_gstate_hasorphans(&lfs->gstate) << 31));

    return 0;
}
#endif

#ifndef RLFS_READONLY
static void rlfs_fs_prepmove(rlfs_t *lfs,
        uint16_t id, const rlfs_block_t pair[2]) {
    lfs->gstate.tag = ((lfs->gstate.tag & ~RLFS_MKTAG(0x7ff, 0x3ff, 0)) |
            ((id != 0x3ff) ? RLFS_MKTAG(RLFS_TYPE_DELETE, id, 0) : 0));
    lfs->gstate.pair[0] = (id != 0x3ff) ? pair[0] : 0;
    lfs->gstate.pair[1] = (id != 0x3ff) ? pair[1] : 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_fs_desuperblock(rlfs_t *lfs) {
    if (!rlfs_gstate_needssuperblock(&lfs->gstate)) {
        return 0;
    }

    RLFS_DEBUG("Rewriting superblock {0x%"PRIx32", 0x%"PRIx32"}",
            lfs->root[0],
            lfs->root[1]);

    rlfs_mdir_t root;
    int err = rlfs_dir_fetch(lfs, &root, lfs->root);
    if (err) {
        return err;
    }

    // write a new superblock
    rlfs_superblock_t superblock = {
        .version     = rlfs_fs_disk_version(lfs),
        .block_size  = lfs->cfg->block_size,
        .block_count = lfs->block_count,
        .name_max    = lfs->name_max,
        .file_max    = lfs->file_max,
        .attr_max    = lfs->attr_max,
    };

    rlfs_superblock_tole32(&superblock);
    err = rlfs_dir_commit(lfs, &root, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                &superblock}));
    if (err) {
        return err;
    }

    rlfs_fs_prepsuperblock(lfs, false);
    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_fs_demove(rlfs_t *lfs) {
    if (!rlfs_gstate_hasmove(&lfs->gdisk)) {
        return 0;
    }

    // Fix bad moves
    RLFS_DEBUG("Fixing move {0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16,
            lfs->gdisk.pair[0],
            lfs->gdisk.pair[1],
            rlfs_tag_id(lfs->gdisk.tag));

    // no other gstate is supported at this time, so if we found something else
    // something most likely went wrong in gstate calculation
    RLFS_ASSERT(rlfs_tag_type3(lfs->gdisk.tag) == RLFS_TYPE_DELETE);

    // fetch and delete the moved entry
    rlfs_mdir_t movedir;
    int err = rlfs_dir_fetch(lfs, &movedir, lfs->gdisk.pair);
    if (err) {
        return err;
    }

    // prep gstate and delete move id
    uint16_t moveid = rlfs_tag_id(lfs->gdisk.tag);
    rlfs_fs_prepmove(lfs, 0x3ff, NULL);
    err = rlfs_dir_commit(lfs, &movedir, RLFS_MKATTRS(
            {RLFS_MKTAG(RLFS_TYPE_DELETE, moveid, 0), NULL}));
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_fs_deorphan(rlfs_t *lfs, bool powerloss) {
    if (!rlfs_gstate_hasorphans(&lfs->gstate)) {
        return 0;
    }

    // Check for orphans in two separate passes:
    // - 1 for half-orphans (relocations)
    // - 2 for full-orphans (removes/renames)
    //
    // Two separate passes are needed as half-orphans can contain outdated
    // references to full-orphans, effectively hiding them from the deorphan
    // search.
    //
    int pass = 0;
    while (pass < 2) {
        // Fix any orphans
        rlfs_mdir_t pdir = {.split = true, .tail = {0, 1}};
        rlfs_mdir_t dir;
        bool moreorphans = false;

        // iterate over all directory directory entries
        while (!rlfs_pair_isnull(pdir.tail)) {
            int err = rlfs_dir_fetch(lfs, &dir, pdir.tail);
            if (err) {
                return err;
            }

            // check head blocks for orphans
            if (!pdir.split) {
                // check if we have a parent
                rlfs_mdir_t parent;
                rlfs_stag_t tag = rlfs_fs_parent(lfs, pdir.tail, &parent);
                if (tag < 0 && tag != RLFS_ERR_NOENT) {
                    return tag;
                }

                if (pass == 0 && tag != RLFS_ERR_NOENT) {
                    rlfs_block_t pair[2];
                    rlfs_stag_t state = rlfs_dir_get(lfs, &parent,
                            RLFS_MKTAG(0x7ff, 0x3ff, 0), tag, pair);
                    if (state < 0) {
                        return state;
                    }
                    rlfs_pair_fromle32(pair);

                    if (!rlfs_pair_issync(pair, pdir.tail)) {
                        // we have desynced
                        RLFS_DEBUG("Fixing half-orphan "
                                "{0x%"PRIx32", 0x%"PRIx32"} "
                                "-> {0x%"PRIx32", 0x%"PRIx32"}",
                                pdir.tail[0], pdir.tail[1], pair[0], pair[1]);

                        // fix pending move in this pair? this looks like an
                        // optimization but is in fact _required_ since
                        // relocating may outdate the move.
                        uint16_t moveid = 0x3ff;
                        if (rlfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
                            moveid = rlfs_tag_id(lfs->gstate.tag);
                            RLFS_DEBUG("Fixing move while fixing orphans "
                                    "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
                                    pdir.pair[0], pdir.pair[1], moveid);
                            rlfs_fs_prepmove(lfs, 0x3ff, NULL);
                        }

                        rlfs_pair_tole32(pair);
                        state = rlfs_dir_orphaningcommit(lfs, &pdir, RLFS_MKATTRS(
                                {RLFS_MKTAG_IF(moveid != 0x3ff,
                                    RLFS_TYPE_DELETE, moveid, 0), NULL},
                                {RLFS_MKTAG(RLFS_TYPE_SOFTTAIL, 0x3ff, 8),
                                    pair}));
                        rlfs_pair_fromle32(pair);
                        if (state < 0) {
                            return state;
                        }

                        // did our commit create more orphans?
                        if (state == RLFS_OK_ORPHANED) {
                            moreorphans = true;
                        }

                        // refetch tail
                        continue;
                    }
                }

                // note we only check for full orphans if we may have had a
                // power-loss, otherwise orphans are created intentionally
                // during operations such as rlfs_mkdir
                if (pass == 1 && tag == RLFS_ERR_NOENT && powerloss) {
                    // we are an orphan
                    RLFS_DEBUG("Fixing orphan {0x%"PRIx32", 0x%"PRIx32"}",
                            pdir.tail[0], pdir.tail[1]);

                    // steal state
                    err = rlfs_dir_getgstate(lfs, &dir, &lfs->gdelta);
                    if (err) {
                        return err;
                    }

                    // steal tail
                    rlfs_pair_tole32(dir.tail);
                    int state = rlfs_dir_orphaningcommit(lfs, &pdir, RLFS_MKATTRS(
                            {RLFS_MKTAG(RLFS_TYPE_TAIL + dir.split, 0x3ff, 8),
                                dir.tail}));
                    rlfs_pair_fromle32(dir.tail);
                    if (state < 0) {
                        return state;
                    }

                    // did our commit create more orphans?
                    if (state == RLFS_OK_ORPHANED) {
                        moreorphans = true;
                    }

                    // refetch tail
                    continue;
                }
            }

            pdir = dir;
        }

        pass = moreorphans ? 0 : pass+1;
    }

    // mark orphans as fixed
    return rlfs_fs_preporphans(lfs, -rlfs_gstate_getorphans(&lfs->gstate));
}
#endif

#ifndef RLFS_READONLY
static int rlfs_fs_forceconsistency(rlfs_t *lfs) {
    int err = rlfs_fs_desuperblock(lfs);
    if (err) {
        return err;
    }

    err = rlfs_fs_demove(lfs);
    if (err) {
        return err;
    }

    err = rlfs_fs_deorphan(lfs, true);
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
static int rlfs_fs_mkconsistent_(rlfs_t *lfs) {
    // rlfs_fs_forceconsistency does most of the work here
    int err = rlfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // do we have any pending gstate?
    rlfs_gstate_t delta = {0};
    rlfs_gstate_xor(&delta, &lfs->gdisk);
    rlfs_gstate_xor(&delta, &lfs->gstate);
    if (!rlfs_gstate_iszero(&delta)) {
        // rlfs_dir_commit will implicitly write out any pending gstate
        rlfs_mdir_t root;
        err = rlfs_dir_fetch(lfs, &root, lfs->root);
        if (err) {
            return err;
        }

        err = rlfs_dir_commit(lfs, &root, NULL, 0);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

static int rlfs_fs_size_count(void *p, rlfs_block_t block) {
    (void)block;
    rlfs_size_t *size = p;
    *size += 1;
    return 0;
}

static rlfs_ssize_t rlfs_fs_size_(rlfs_t *lfs) {
    rlfs_size_t size = 0;
    int err = rlfs_fs_traverse_(lfs, rlfs_fs_size_count, &size, false);
    if (err) {
        return err;
    }

    return size;
}

// explicit garbage collection
#ifndef RLFS_READONLY
static int rlfs_fs_gc_(rlfs_t *lfs) {
    // force consistency, even if we're not necessarily going to write,
    // because this function is supposed to take care of janitorial work
    // isn't it?
    int err = rlfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // try to compact metadata pairs, note we can't really accomplish
    // anything if compact_thresh doesn't at least leave a prog_size
    // available
    if (lfs->cfg->compact_thresh
            < lfs->cfg->block_size - lfs->cfg->prog_size) {
        // iterate over all mdirs
        rlfs_mdir_t mdir = {.tail = {0, 1}};
        while (!rlfs_pair_isnull(mdir.tail)) {
            err = rlfs_dir_fetch(lfs, &mdir, mdir.tail);
            if (err) {
                return err;
            }

            // not erased? exceeds our compaction threshold?
            if (!mdir.erased || ((lfs->cfg->compact_thresh == 0)
                    ? mdir.off > lfs->cfg->block_size - lfs->cfg->block_size/8
                    : mdir.off > lfs->cfg->compact_thresh)) {
                // the easiest way to trigger a compaction is to mark
                // the mdir as unerased and add an empty commit
                mdir.erased = false;
                err = rlfs_dir_commit(lfs, &mdir, NULL, 0);
                if (err) {
                    return err;
                }
            }
        }
    }

    // try to populate the lookahead buffer, unless it's already full
    if (lfs->lookahead.size < rlfs_min(
            8 * lfs->cfg->lookahead_size,
            lfs->block_count)) {
        err = rlfs_alloc_scan(lfs);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef RLFS_READONLY
#ifdef RLFS_SHRINKNONRELOCATING
static int rlfs_shrink_checkblock(void *data, rlfs_block_t block) {
    rlfs_size_t threshold = *((rlfs_size_t*)data);
    if (block >= threshold) {
        return RLFS_ERR_NOTEMPTY;
    }
    return 0;
}
#endif

static int rlfs_fs_grow_(rlfs_t *lfs, rlfs_size_t block_count) {
    int err;

    if (block_count == lfs->block_count) {
        return 0;
    }

    
#ifndef RLFS_SHRINKNONRELOCATING
    // shrinking is not supported
    RLFS_ASSERT(block_count >= lfs->block_count);
#endif
#ifdef RLFS_SHRINKNONRELOCATING
    if (block_count < lfs->block_count) {
        err = rlfs_fs_traverse_(lfs, rlfs_shrink_checkblock, &block_count, true);
        if (err) {
            return err;
        }
    }
#endif

    lfs->block_count = block_count;

    // fetch the root
    rlfs_mdir_t root;
    err = rlfs_dir_fetch(lfs, &root, lfs->root);
    if (err) {
        return err;
    }

    // update the superblock
    rlfs_superblock_t superblock;
    rlfs_stag_t tag = rlfs_dir_get(lfs, &root, RLFS_MKTAG(0x7ff, 0x3ff, 0),
            RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
            &superblock);
    if (tag < 0) {
        return tag;
    }
    rlfs_superblock_fromle32(&superblock);

    superblock.block_count = lfs->block_count;

    rlfs_superblock_tole32(&superblock);
    err = rlfs_dir_commit(lfs, &root, RLFS_MKATTRS(
            {tag, &superblock}));
    if (err) {
        return err;
    }
    return 0;
}
#endif

#ifdef RLFS_MIGRATE
////// Migration from littelfs v1 below this //////

/// Version info ///

// Software library version
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS1_VERSION 0x00010007
#define LFS1_VERSION_MAJOR (0xffff & (LFS1_VERSION >> 16))
#define LFS1_VERSION_MINOR (0xffff & (LFS1_VERSION >>  0))

// Version of On-disk data structures
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS1_DISK_VERSION 0x00010001
#define LFS1_DISK_VERSION_MAJOR (0xffff & (LFS1_DISK_VERSION >> 16))
#define LFS1_DISK_VERSION_MINOR (0xffff & (LFS1_DISK_VERSION >>  0))


/// v1 Definitions ///

// File types
enum lfs1_type {
    LFS1_TYPE_REG        = 0x11,
    LFS1_TYPE_DIR        = 0x22,
    LFS1_TYPE_SUPERBLOCK = 0x2e,
};

typedef struct lfs1 {
    rlfs_block_t root[2];
} lfs1_t;

typedef struct lfs1_entry {
    rlfs_off_t off;

    struct lfs1_disk_entry {
        uint8_t type;
        uint8_t elen;
        uint8_t alen;
        uint8_t nlen;
        union {
            struct {
                rlfs_block_t head;
                rlfs_size_t size;
            } file;
            rlfs_block_t dir[2];
        } u;
    } d;
} lfs1_entry_t;

typedef struct lfs1_dir {
    struct lfs1_dir *next;
    rlfs_block_t pair[2];
    rlfs_off_t off;

    rlfs_block_t head[2];
    rlfs_off_t pos;

    struct lfs1_disk_dir {
        uint32_t rev;
        rlfs_size_t size;
        rlfs_block_t tail[2];
    } d;
} lfs1_dir_t;

typedef struct lfs1_superblock {
    rlfs_off_t off;

    struct lfs1_disk_superblock {
        uint8_t type;
        uint8_t elen;
        uint8_t alen;
        uint8_t nlen;
        rlfs_block_t root[2];
        uint32_t block_size;
        uint32_t block_count;
        uint32_t version;
        char magic[8];
    } d;
} lfs1_superblock_t;


/// Low-level wrappers v1->v2 ///
static void lfs1_crc(uint32_t *crc, const void *buffer, size_t size) {
    *crc = rlfs_crc(*crc, buffer, size);
}

static int lfs1_bd_read(rlfs_t *lfs, rlfs_block_t block,
        rlfs_off_t off, void *buffer, rlfs_size_t size) {
    // if we ever do more than writes to alternating pairs,
    // this may need to consider pcache
    return rlfs_bd_read(lfs, &lfs->pcache, &lfs->rcache, size,
            block, off, buffer, size);
}

static int lfs1_bd_crc(rlfs_t *lfs, rlfs_block_t block,
        rlfs_off_t off, rlfs_size_t size, uint32_t *crc) {
    for (rlfs_off_t i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs1_bd_read(lfs, block, off+i, &c, 1);
        if (err) {
            return err;
        }

        lfs1_crc(crc, &c, 1);
    }

    return 0;
}


/// Endian swapping functions ///
static void lfs1_dir_fromle32(struct lfs1_disk_dir *d) {
    d->rev     = rlfs_fromle32(d->rev);
    d->size    = rlfs_fromle32(d->size);
    d->tail[0] = rlfs_fromle32(d->tail[0]);
    d->tail[1] = rlfs_fromle32(d->tail[1]);
}

static void lfs1_dir_tole32(struct lfs1_disk_dir *d) {
    d->rev     = rlfs_tole32(d->rev);
    d->size    = rlfs_tole32(d->size);
    d->tail[0] = rlfs_tole32(d->tail[0]);
    d->tail[1] = rlfs_tole32(d->tail[1]);
}

static void lfs1_entry_fromle32(struct lfs1_disk_entry *d) {
    d->u.dir[0] = rlfs_fromle32(d->u.dir[0]);
    d->u.dir[1] = rlfs_fromle32(d->u.dir[1]);
}

static void lfs1_entry_tole32(struct lfs1_disk_entry *d) {
    d->u.dir[0] = rlfs_tole32(d->u.dir[0]);
    d->u.dir[1] = rlfs_tole32(d->u.dir[1]);
}

static void lfs1_superblock_fromle32(struct lfs1_disk_superblock *d) {
    d->root[0]     = rlfs_fromle32(d->root[0]);
    d->root[1]     = rlfs_fromle32(d->root[1]);
    d->block_size  = rlfs_fromle32(d->block_size);
    d->block_count = rlfs_fromle32(d->block_count);
    d->version     = rlfs_fromle32(d->version);
}


///// Metadata pair and directory operations ///
static inline rlfs_size_t lfs1_entry_size(const lfs1_entry_t *entry) {
    return 4 + entry->d.elen + entry->d.alen + entry->d.nlen;
}

static int lfs1_dir_fetch(rlfs_t *lfs,
        lfs1_dir_t *dir, const rlfs_block_t pair[2]) {
    // copy out pair, otherwise may be aliasing dir
    const rlfs_block_t tpair[2] = {pair[0], pair[1]};
    bool valid = false;

    // check both blocks for the most recent revision
    for (int i = 0; i < 2; i++) {
        struct lfs1_disk_dir test;
        int err = lfs1_bd_read(lfs, tpair[i], 0, &test, sizeof(test));
        lfs1_dir_fromle32(&test);
        if (err) {
            if (err == RLFS_ERR_CORRUPT) {
                continue;
            }
            return err;
        }

        if (valid && rlfs_scmp(test.rev, dir->d.rev) < 0) {
            continue;
        }

        if ((0x7fffffff & test.size) < sizeof(test)+4 ||
            (0x7fffffff & test.size) > lfs->cfg->block_size) {
            continue;
        }

        uint32_t crc = 0xffffffff;
        lfs1_dir_tole32(&test);
        lfs1_crc(&crc, &test, sizeof(test));
        lfs1_dir_fromle32(&test);
        err = lfs1_bd_crc(lfs, tpair[i], sizeof(test),
                (0x7fffffff & test.size) - sizeof(test), &crc);
        if (err) {
            if (err == RLFS_ERR_CORRUPT) {
                continue;
            }
            return err;
        }

        if (crc != 0) {
            continue;
        }

        valid = true;

        // setup dir in case it's valid
        dir->pair[0] = tpair[(i+0) % 2];
        dir->pair[1] = tpair[(i+1) % 2];
        dir->off = sizeof(dir->d);
        dir->d = test;
    }

    if (!valid) {
        RLFS_ERROR("Corrupted dir pair at {0x%"PRIx32", 0x%"PRIx32"}",
                tpair[0], tpair[1]);
        return RLFS_ERR_CORRUPT;
    }

    return 0;
}

static int lfs1_dir_next(rlfs_t *lfs, lfs1_dir_t *dir, lfs1_entry_t *entry) {
    while (dir->off + sizeof(entry->d) > (0x7fffffff & dir->d.size)-4) {
        if (!(0x80000000 & dir->d.size)) {
            entry->off = dir->off;
            return RLFS_ERR_NOENT;
        }

        int err = lfs1_dir_fetch(lfs, dir, dir->d.tail);
        if (err) {
            return err;
        }

        dir->off = sizeof(dir->d);
        dir->pos += sizeof(dir->d) + 4;
    }

    int err = lfs1_bd_read(lfs, dir->pair[0], dir->off,
            &entry->d, sizeof(entry->d));
    lfs1_entry_fromle32(&entry->d);
    if (err) {
        return err;
    }

    entry->off = dir->off;
    dir->off += lfs1_entry_size(entry);
    dir->pos += lfs1_entry_size(entry);
    return 0;
}

/// littlefs v1 specific operations ///
int lfs1_traverse(rlfs_t *lfs, int (*cb)(void*, rlfs_block_t), void *data) {
    if (rlfs_pair_isnull(lfs->lfs1->root)) {
        return 0;
    }

    // iterate over metadata pairs
    lfs1_dir_t dir;
    lfs1_entry_t entry;
    rlfs_block_t cwd[2] = {0, 1};

    while (true) {
        for (int i = 0; i < 2; i++) {
            int err = cb(data, cwd[i]);
            if (err) {
                return err;
            }
        }

        int err = lfs1_dir_fetch(lfs, &dir, cwd);
        if (err) {
            return err;
        }

        // iterate over contents
        while (dir.off + sizeof(entry.d) <= (0x7fffffff & dir.d.size)-4) {
            err = lfs1_bd_read(lfs, dir.pair[0], dir.off,
                    &entry.d, sizeof(entry.d));
            lfs1_entry_fromle32(&entry.d);
            if (err) {
                return err;
            }

            dir.off += lfs1_entry_size(&entry);
            if ((0x70 & entry.d.type) == (0x70 & LFS1_TYPE_REG)) {
                err = rlfs_ctz_traverse(lfs, NULL, &lfs->rcache,
                        entry.d.u.file.head, entry.d.u.file.size, cb, data);
                if (err) {
                    return err;
                }
            }
        }

        // we also need to check if we contain a threaded v2 directory
        rlfs_mdir_t dir2 = {.split=true, .tail={cwd[0], cwd[1]}};
        while (dir2.split) {
            err = rlfs_dir_fetch(lfs, &dir2, dir2.tail);
            if (err) {
                break;
            }

            for (int i = 0; i < 2; i++) {
                err = cb(data, dir2.pair[i]);
                if (err) {
                    return err;
                }
            }
        }

        cwd[0] = dir.d.tail[0];
        cwd[1] = dir.d.tail[1];

        if (rlfs_pair_isnull(cwd)) {
            break;
        }
    }

    return 0;
}

static int lfs1_moved(rlfs_t *lfs, const void *e) {
    if (rlfs_pair_isnull(lfs->lfs1->root)) {
        return 0;
    }

    // skip superblock
    lfs1_dir_t cwd;
    int err = lfs1_dir_fetch(lfs, &cwd, (const rlfs_block_t[2]){0, 1});
    if (err) {
        return err;
    }

    // iterate over all directory directory entries
    lfs1_entry_t entry;
    while (!rlfs_pair_isnull(cwd.d.tail)) {
        err = lfs1_dir_fetch(lfs, &cwd, cwd.d.tail);
        if (err) {
            return err;
        }

        while (true) {
            err = lfs1_dir_next(lfs, &cwd, &entry);
            if (err && err != RLFS_ERR_NOENT) {
                return err;
            }

            if (err == RLFS_ERR_NOENT) {
                break;
            }

            if (!(0x80 & entry.d.type) &&
                 memcmp(&entry.d.u, e, sizeof(entry.d.u)) == 0) {
                return true;
            }
        }
    }

    return false;
}

/// Filesystem operations ///
static int lfs1_mount(rlfs_t *lfs, struct lfs1 *lfs1,
        const struct rlfs_config *cfg) {
    int err = 0;
    {
        err = rlfs_init(lfs, cfg);
        if (err) {
            return err;
        }

        lfs->lfs1 = lfs1;
        lfs->lfs1->root[0] = RLFS_BLOCK_NULL;
        lfs->lfs1->root[1] = RLFS_BLOCK_NULL;

        // setup free lookahead
        lfs->lookahead.start = 0;
        lfs->lookahead.size = 0;
        lfs->lookahead.next = 0;
        rlfs_alloc_ckpoint(lfs);

        // load superblock
        lfs1_dir_t dir;
        lfs1_superblock_t superblock;
        err = lfs1_dir_fetch(lfs, &dir, (const rlfs_block_t[2]){0, 1});
        if (err && err != RLFS_ERR_CORRUPT) {
            goto cleanup;
        }

        if (!err) {
            err = lfs1_bd_read(lfs, dir.pair[0], sizeof(dir.d),
                    &superblock.d, sizeof(superblock.d));
            lfs1_superblock_fromle32(&superblock.d);
            if (err) {
                goto cleanup;
            }

            lfs->lfs1->root[0] = superblock.d.root[0];
            lfs->lfs1->root[1] = superblock.d.root[1];
        }

        if (err || memcmp(superblock.d.magic, "littlefs", 8) != 0) {
            RLFS_ERROR("Invalid superblock at {0x%"PRIx32", 0x%"PRIx32"}",
                    0, 1);
            err = RLFS_ERR_CORRUPT;
            goto cleanup;
        }

        uint16_t major_version = (0xffff & (superblock.d.version >> 16));
        uint16_t minor_version = (0xffff & (superblock.d.version >>  0));
        if ((major_version != LFS1_DISK_VERSION_MAJOR ||
             minor_version > LFS1_DISK_VERSION_MINOR)) {
            RLFS_ERROR("Invalid version v%d.%d", major_version, minor_version);
            err = RLFS_ERR_INVAL;
            goto cleanup;
        }

        return 0;
    }

cleanup:
    rlfs_deinit(lfs);
    return err;
}

static int lfs1_unmount(rlfs_t *lfs) {
    return rlfs_deinit(lfs);
}

/// v1 migration ///
static int rlfs_migrate_(rlfs_t *lfs, const struct rlfs_config *cfg) {
    struct lfs1 lfs1;

    // Indeterminate filesystem size not allowed for migration.
    RLFS_ASSERT(cfg->block_count != 0);

    int err = lfs1_mount(lfs, &lfs1, cfg);
    if (err) {
        return err;
    }

    {
        // iterate through each directory, copying over entries
        // into new directory
        lfs1_dir_t dir1;
        rlfs_mdir_t dir2;
        dir1.d.tail[0] = lfs->lfs1->root[0];
        dir1.d.tail[1] = lfs->lfs1->root[1];
        while (!rlfs_pair_isnull(dir1.d.tail)) {
            // iterate old dir
            err = lfs1_dir_fetch(lfs, &dir1, dir1.d.tail);
            if (err) {
                goto cleanup;
            }

            // create new dir and bind as temporary pretend root
            err = rlfs_dir_alloc(lfs, &dir2);
            if (err) {
                goto cleanup;
            }

            dir2.rev = dir1.d.rev;
            dir1.head[0] = dir1.pair[0];
            dir1.head[1] = dir1.pair[1];
            lfs->root[0] = dir2.pair[0];
            lfs->root[1] = dir2.pair[1];

            err = rlfs_dir_commit(lfs, &dir2, NULL, 0);
            if (err) {
                goto cleanup;
            }

            while (true) {
                lfs1_entry_t entry1;
                err = lfs1_dir_next(lfs, &dir1, &entry1);
                if (err && err != RLFS_ERR_NOENT) {
                    goto cleanup;
                }

                if (err == RLFS_ERR_NOENT) {
                    break;
                }

                // check that entry has not been moved
                if (entry1.d.type & 0x80) {
                    int moved = lfs1_moved(lfs, &entry1.d.u);
                    if (moved < 0) {
                        err = moved;
                        goto cleanup;
                    }

                    if (moved) {
                        continue;
                    }

                    entry1.d.type &= ~0x80;
                }

                // also fetch name
                char name[RLFS_NAME_MAX+1];
                memset(name, 0, sizeof(name));
                err = lfs1_bd_read(lfs, dir1.pair[0],
                        entry1.off + 4+entry1.d.elen+entry1.d.alen,
                        name, entry1.d.nlen);
                if (err) {
                    goto cleanup;
                }

                bool isdir = (entry1.d.type == LFS1_TYPE_DIR);

                // create entry in new dir
                err = rlfs_dir_fetch(lfs, &dir2, lfs->root);
                if (err) {
                    goto cleanup;
                }

                uint16_t id;
                err = rlfs_dir_find(lfs, &dir2, &(const char*){name}, &id);
                if (!(err == RLFS_ERR_NOENT && id != 0x3ff)) {
                    err = (err < 0) ? err : RLFS_ERR_EXIST;
                    goto cleanup;
                }

                lfs1_entry_tole32(&entry1.d);
                err = rlfs_dir_commit(lfs, &dir2, RLFS_MKATTRS(
                        {RLFS_MKTAG(RLFS_TYPE_CREATE, id, 0), NULL},
                        {RLFS_MKTAG_IF_ELSE(isdir,
                            RLFS_TYPE_DIR, id, entry1.d.nlen,
                            RLFS_TYPE_REG, id, entry1.d.nlen),
                                name},
                        {RLFS_MKTAG_IF_ELSE(isdir,
                            RLFS_TYPE_DIRSTRUCT, id, sizeof(entry1.d.u),
                            RLFS_TYPE_CTZSTRUCT, id, sizeof(entry1.d.u)),
                                &entry1.d.u}));
                lfs1_entry_fromle32(&entry1.d);
                if (err) {
                    goto cleanup;
                }
            }

            if (!rlfs_pair_isnull(dir1.d.tail)) {
                // find last block and update tail to thread into fs
                err = rlfs_dir_fetch(lfs, &dir2, lfs->root);
                if (err) {
                    goto cleanup;
                }

                while (dir2.split) {
                    err = rlfs_dir_fetch(lfs, &dir2, dir2.tail);
                    if (err) {
                        goto cleanup;
                    }
                }

                rlfs_pair_tole32(dir2.pair);
                err = rlfs_dir_commit(lfs, &dir2, RLFS_MKATTRS(
                        {RLFS_MKTAG(RLFS_TYPE_SOFTTAIL, 0x3ff, 8), dir1.d.tail}));
                rlfs_pair_fromle32(dir2.pair);
                if (err) {
                    goto cleanup;
                }
            }

            // Copy over first block to thread into fs. Unfortunately
            // if this fails there is not much we can do.
            RLFS_DEBUG("Migrating {0x%"PRIx32", 0x%"PRIx32"} "
                        "-> {0x%"PRIx32", 0x%"PRIx32"}",
                    lfs->root[0], lfs->root[1], dir1.head[0], dir1.head[1]);

            err = rlfs_bd_erase(lfs, dir1.head[1]);
            if (err) {
                goto cleanup;
            }

            err = rlfs_dir_fetch(lfs, &dir2, lfs->root);
            if (err) {
                goto cleanup;
            }

            for (rlfs_off_t i = 0; i < dir2.off; i++) {
                uint8_t dat;
                err = rlfs_bd_read(lfs,
                        NULL, &lfs->rcache, dir2.off,
                        dir2.pair[0], i, &dat, 1);
                if (err) {
                    goto cleanup;
                }

                err = rlfs_bd_prog(lfs,
                        &lfs->pcache, &lfs->rcache, true,
                        dir1.head[1], i, &dat, 1);
                if (err) {
                    goto cleanup;
                }
            }

            err = rlfs_bd_flush(lfs, &lfs->pcache, &lfs->rcache, true);
            if (err) {
                goto cleanup;
            }
        }

        // Create new superblock. This marks a successful migration!
        err = lfs1_dir_fetch(lfs, &dir1, (const rlfs_block_t[2]){0, 1});
        if (err) {
            goto cleanup;
        }

        dir2.pair[0] = dir1.pair[0];
        dir2.pair[1] = dir1.pair[1];
        dir2.rev = dir1.d.rev;
        dir2.off = sizeof(dir2.rev);
        dir2.etag = 0xffffffff;
        dir2.count = 0;
        dir2.tail[0] = lfs->lfs1->root[0];
        dir2.tail[1] = lfs->lfs1->root[1];
        dir2.erased = false;
        dir2.split = true;

        rlfs_superblock_t superblock = {
            .version     = RLFS_DISK_VERSION,
            .block_size  = lfs->cfg->block_size,
            .block_count = lfs->cfg->block_count,
            .name_max    = lfs->name_max,
            .file_max    = lfs->file_max,
            .attr_max    = lfs->attr_max,
        };

        rlfs_superblock_tole32(&superblock);
        err = rlfs_dir_commit(lfs, &dir2, RLFS_MKATTRS(
                {RLFS_MKTAG(RLFS_TYPE_CREATE, 0, 0), NULL},
                {RLFS_MKTAG(RLFS_TYPE_SUPERBLOCK, 0, 8), "littlefs"},
                {RLFS_MKTAG(RLFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                    &superblock}));
        if (err) {
            goto cleanup;
        }

        // sanity check that fetch works
        err = rlfs_dir_fetch(lfs, &dir2, (const rlfs_block_t[2]){0, 1});
        if (err) {
            goto cleanup;
        }

        // force compaction to prevent accidentally mounting v1
        dir2.erased = false;
        err = rlfs_dir_commit(lfs, &dir2, NULL, 0);
        if (err) {
            goto cleanup;
        }
    }

cleanup:
    lfs1_unmount(lfs);
    return err;
}

#endif


/// Public API wrappers ///

// Here we can add tracing/thread safety easily

// Thread-safe wrappers if enabled
#ifdef RLFS_THREADSAFE
#define RLFS_LOCK(cfg)   cfg->lock(cfg)
#define RLFS_UNLOCK(cfg) cfg->unlock(cfg)
#else
#define RLFS_LOCK(cfg)   ((void)cfg, 0)
#define RLFS_UNLOCK(cfg) ((void)cfg)
#endif

// Public API
#ifndef RLFS_READONLY
int rlfs_format(rlfs_t *lfs, const struct rlfs_config *cfg) {
    int err = RLFS_LOCK(cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_format(%p, %p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
                ".block_cycles=%"PRId32", .cache_size=%"PRIu32", "
                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
                ".prog_buffer=%p, .lookahead_buffer=%p, "
                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
                ".attr_max=%"PRIu32"})",
            (void*)lfs, (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
            cfg->name_max, cfg->file_max, cfg->attr_max);

    err = rlfs_format_(lfs, cfg);

    RLFS_TRACE("rlfs_format -> %d", err);
    RLFS_UNLOCK(cfg);
    return err;
}
#endif

int rlfs_mount(rlfs_t *lfs, const struct rlfs_config *cfg) {
    int err = RLFS_LOCK(cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_mount(%p, %p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
                ".block_cycles=%"PRId32", .cache_size=%"PRIu32", "
                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
                ".prog_buffer=%p, .lookahead_buffer=%p, "
                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
                ".attr_max=%"PRIu32"})",
            (void*)lfs, (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
            cfg->name_max, cfg->file_max, cfg->attr_max);

    err = rlfs_mount_(lfs, cfg);

    RLFS_TRACE("rlfs_mount -> %d", err);
    RLFS_UNLOCK(cfg);
    return err;
}

int rlfs_unmount(rlfs_t *lfs) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_unmount(%p)", (void*)lfs);

    err = rlfs_unmount_(lfs);

    RLFS_TRACE("rlfs_unmount -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

#ifndef RLFS_READONLY
int rlfs_remove(rlfs_t *lfs, const char *path) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_remove(%p, \"%s\")", (void*)lfs, path);

    err = rlfs_remove_(lfs, path);

    RLFS_TRACE("rlfs_remove -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef RLFS_READONLY
int rlfs_rename(rlfs_t *lfs, const char *oldpath, const char *newpath) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_rename(%p, \"%s\", \"%s\")", (void*)lfs, oldpath, newpath);

    err = rlfs_rename_(lfs, oldpath, newpath);

    RLFS_TRACE("rlfs_rename -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

int rlfs_stat(rlfs_t *lfs, const char *path, struct rlfs_info *info) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_stat(%p, \"%s\", %p)", (void*)lfs, path, (void*)info);

    err = rlfs_stat_(lfs, path, info);

    RLFS_TRACE("rlfs_stat -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

rlfs_ssize_t rlfs_getattr(rlfs_t *lfs, const char *path,
        uint8_t type, void *buffer, rlfs_size_t size) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_getattr(%p, \"%s\", %"PRIu8", %p, %"PRIu32")",
            (void*)lfs, path, type, buffer, size);

    rlfs_ssize_t res = rlfs_getattr_(lfs, path, type, buffer, size);

    RLFS_TRACE("rlfs_getattr -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef RLFS_READONLY
int rlfs_setattr(rlfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, rlfs_size_t size) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_setattr(%p, \"%s\", %"PRIu8", %p, %"PRIu32")",
            (void*)lfs, path, type, buffer, size);

    err = rlfs_setattr_(lfs, path, type, buffer, size);

    RLFS_TRACE("rlfs_setattr -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef RLFS_READONLY
int rlfs_removeattr(rlfs_t *lfs, const char *path, uint8_t type) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_removeattr(%p, \"%s\", %"PRIu8")", (void*)lfs, path, type);

    err = rlfs_removeattr_(lfs, path, type);

    RLFS_TRACE("rlfs_removeattr -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef RLFS_NO_MALLOC
int rlfs_file_open(rlfs_t *lfs, rlfs_file_t *file, const char *path, int flags) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_open(%p, %p, \"%s\", %x)",
            (void*)lfs, (void*)file, path, (unsigned)flags);
    RLFS_ASSERT(!rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    err = rlfs_file_open_(lfs, file, path, flags);

    RLFS_TRACE("rlfs_file_open -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

int rlfs_file_opencfg(rlfs_t *lfs, rlfs_file_t *file,
        const char *path, int flags,
        const struct rlfs_file_config *cfg) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_opencfg(%p, %p, \"%s\", %x, %p {"
                 ".buffer=%p, .attrs=%p, .attr_count=%"PRIu32"})",
            (void*)lfs, (void*)file, path, (unsigned)flags,
            (void*)cfg, cfg->buffer, (void*)cfg->attrs, cfg->attr_count);
    RLFS_ASSERT(!rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    err = rlfs_file_opencfg_(lfs, file, path, flags, cfg);

    RLFS_TRACE("rlfs_file_opencfg -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

int rlfs_file_close(rlfs_t *lfs, rlfs_file_t *file) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_close(%p, %p)", (void*)lfs, (void*)file);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    err = rlfs_file_close_(lfs, file);

    RLFS_TRACE("rlfs_file_close -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

#ifndef RLFS_READONLY
int rlfs_file_sync(rlfs_t *lfs, rlfs_file_t *file) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_sync(%p, %p)", (void*)lfs, (void*)file);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    err = rlfs_file_sync_(lfs, file);

    RLFS_TRACE("rlfs_file_sync -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

rlfs_ssize_t rlfs_file_read(rlfs_t *lfs, rlfs_file_t *file,
        void *buffer, rlfs_size_t size) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_read(%p, %p, %p, %"PRIu32")",
            (void*)lfs, (void*)file, buffer, size);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    rlfs_ssize_t res = rlfs_file_read_(lfs, file, buffer, size);

    RLFS_TRACE("rlfs_file_read -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef RLFS_READONLY
rlfs_ssize_t rlfs_file_write(rlfs_t *lfs, rlfs_file_t *file,
        const void *buffer, rlfs_size_t size) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_write(%p, %p, %p, %"PRIu32")",
            (void*)lfs, (void*)file, buffer, size);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    rlfs_ssize_t res = rlfs_file_write_(lfs, file, buffer, size);

    RLFS_TRACE("rlfs_file_write -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}
#endif

rlfs_soff_t rlfs_file_seek(rlfs_t *lfs, rlfs_file_t *file,
        rlfs_soff_t off, int whence) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_seek(%p, %p, %"PRId32", %d)",
            (void*)lfs, (void*)file, off, whence);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    rlfs_soff_t res = rlfs_file_seek_(lfs, file, off, whence);

    RLFS_TRACE("rlfs_file_seek -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef RLFS_READONLY
int rlfs_file_truncate(rlfs_t *lfs, rlfs_file_t *file, rlfs_off_t size) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_truncate(%p, %p, %"PRIu32")",
            (void*)lfs, (void*)file, size);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    err = rlfs_file_truncate_(lfs, file, size);

    RLFS_TRACE("rlfs_file_truncate -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

rlfs_soff_t rlfs_file_tell(rlfs_t *lfs, rlfs_file_t *file) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_tell(%p, %p)", (void*)lfs, (void*)file);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    rlfs_soff_t res = rlfs_file_tell_(lfs, file);

    RLFS_TRACE("rlfs_file_tell -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

int rlfs_file_rewind(rlfs_t *lfs, rlfs_file_t *file) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_rewind(%p, %p)", (void*)lfs, (void*)file);

    err = rlfs_file_rewind_(lfs, file);

    RLFS_TRACE("rlfs_file_rewind -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

rlfs_soff_t rlfs_file_size(rlfs_t *lfs, rlfs_file_t *file) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_file_size(%p, %p)", (void*)lfs, (void*)file);
    RLFS_ASSERT(rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)file));

    rlfs_soff_t res = rlfs_file_size_(lfs, file);

    RLFS_TRACE("rlfs_file_size -> %"PRIu32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef RLFS_READONLY
int rlfs_mkdir(rlfs_t *lfs, const char *path) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_mkdir(%p, \"%s\")", (void*)lfs, path);

    err = rlfs_mkdir_(lfs, path);

    RLFS_TRACE("rlfs_mkdir -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

int rlfs_dir_open(rlfs_t *lfs, rlfs_dir_t *dir, const char *path) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_dir_open(%p, %p, \"%s\")", (void*)lfs, (void*)dir, path);
    RLFS_ASSERT(!rlfs_mlist_isopen(lfs->mlist, (struct rlfs_mlist*)dir));

    err = rlfs_dir_open_(lfs, dir, path);

    RLFS_TRACE("rlfs_dir_open -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

int rlfs_dir_close(rlfs_t *lfs, rlfs_dir_t *dir) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_dir_close(%p, %p)", (void*)lfs, (void*)dir);

    err = rlfs_dir_close_(lfs, dir);

    RLFS_TRACE("rlfs_dir_close -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

int rlfs_dir_read(rlfs_t *lfs, rlfs_dir_t *dir, struct rlfs_info *info) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_dir_read(%p, %p, %p)",
            (void*)lfs, (void*)dir, (void*)info);

    err = rlfs_dir_read_(lfs, dir, info);

    RLFS_TRACE("rlfs_dir_read -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

int rlfs_dir_seek(rlfs_t *lfs, rlfs_dir_t *dir, rlfs_off_t off) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_dir_seek(%p, %p, %"PRIu32")",
            (void*)lfs, (void*)dir, off);

    err = rlfs_dir_seek_(lfs, dir, off);

    RLFS_TRACE("rlfs_dir_seek -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

rlfs_soff_t rlfs_dir_tell(rlfs_t *lfs, rlfs_dir_t *dir) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_dir_tell(%p, %p)", (void*)lfs, (void*)dir);

    rlfs_soff_t res = rlfs_dir_tell_(lfs, dir);

    RLFS_TRACE("rlfs_dir_tell -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

int rlfs_dir_rewind(rlfs_t *lfs, rlfs_dir_t *dir) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_dir_rewind(%p, %p)", (void*)lfs, (void*)dir);

    err = rlfs_dir_rewind_(lfs, dir);

    RLFS_TRACE("rlfs_dir_rewind -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

int rlfs_fs_stat(rlfs_t *lfs, struct rlfs_fsinfo *fsinfo) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_fs_stat(%p, %p)", (void*)lfs, (void*)fsinfo);

    err = rlfs_fs_stat_(lfs, fsinfo);

    RLFS_TRACE("rlfs_fs_stat -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

rlfs_ssize_t rlfs_fs_size(rlfs_t *lfs) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_fs_size(%p)", (void*)lfs);

    rlfs_ssize_t res = rlfs_fs_size_(lfs);

    RLFS_TRACE("rlfs_fs_size -> %"PRId32, res);
    RLFS_UNLOCK(lfs->cfg);
    return res;
}

int rlfs_fs_traverse(rlfs_t *lfs, int (*cb)(void *, rlfs_block_t), void *data) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_fs_traverse(%p, %p, %p)",
            (void*)lfs, (void*)(uintptr_t)cb, data);

    err = rlfs_fs_traverse_(lfs, cb, data, true);

    RLFS_TRACE("rlfs_fs_traverse -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}

#ifndef RLFS_READONLY
int rlfs_fs_mkconsistent(rlfs_t *lfs) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_fs_mkconsistent(%p)", (void*)lfs);

    err = rlfs_fs_mkconsistent_(lfs);

    RLFS_TRACE("rlfs_fs_mkconsistent -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef RLFS_READONLY
int rlfs_fs_gc(rlfs_t *lfs) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_fs_gc(%p)", (void*)lfs);

    err = rlfs_fs_gc_(lfs);

    RLFS_TRACE("rlfs_fs_gc -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef RLFS_READONLY
int rlfs_fs_grow(rlfs_t *lfs, rlfs_size_t block_count) {
    int err = RLFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_fs_grow(%p, %"PRIu32")", (void*)lfs, block_count);

    err = rlfs_fs_grow_(lfs, block_count);

    RLFS_TRACE("rlfs_fs_grow -> %d", err);
    RLFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifdef RLFS_MIGRATE
int rlfs_migrate(rlfs_t *lfs, const struct rlfs_config *cfg) {
    int err = RLFS_LOCK(cfg);
    if (err) {
        return err;
    }
    RLFS_TRACE("rlfs_migrate(%p, %p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
                ".block_cycles=%"PRId32", .cache_size=%"PRIu32", "
                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
                ".prog_buffer=%p, .lookahead_buffer=%p, "
                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
                ".attr_max=%"PRIu32"})",
            (void*)lfs, (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
            cfg->name_max, cfg->file_max, cfg->attr_max);

    err = rlfs_migrate_(lfs, cfg);

    RLFS_TRACE("rlfs_migrate -> %d", err);
    RLFS_UNLOCK(cfg);
    return err;
}
#endif

