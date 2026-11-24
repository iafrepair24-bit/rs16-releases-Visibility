/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RLFS_H
#define RLFS_H

#include "rlfs_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


/// Version info ///

// Software library version
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define RLFS_VERSION 0x0002000b
#define RLFS_VERSION_MAJOR (0xffff & (RLFS_VERSION >> 16))
#define RLFS_VERSION_MINOR (0xffff & (RLFS_VERSION >>  0))

// Version of On-disk data structures
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define RLFS_DISK_VERSION 0x00020001
#define RLFS_DISK_VERSION_MAJOR (0xffff & (RLFS_DISK_VERSION >> 16))
#define RLFS_DISK_VERSION_MINOR (0xffff & (RLFS_DISK_VERSION >>  0))


/// Definitions ///

// Type definitions
typedef uint32_t rlfs_size_t;
typedef uint32_t rlfs_off_t;

typedef int32_t  rlfs_ssize_t;
typedef int32_t  rlfs_soff_t;

typedef uint32_t rlfs_block_t;

// Maximum name size in bytes, may be redefined to reduce the size of the
// info struct. Limited to <= 1022. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef RLFS_NAME_MAX
#define RLFS_NAME_MAX 255
#endif

// Maximum size of a file in bytes, may be redefined to limit to support other
// drivers. Limited on disk to <= 2147483647. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef RLFS_FILE_MAX
#define RLFS_FILE_MAX 2147483647
#endif

// Maximum size of custom attributes in bytes, may be redefined, but there is
// no real benefit to using a smaller RLFS_ATTR_MAX. Limited to <= 1022. Stored
// in superblock and must be respected by other littlefs drivers.
#ifndef RLFS_ATTR_MAX
#define RLFS_ATTR_MAX 1022
#endif

// Possible error codes, these are negative to allow
// valid positive return values
enum rlfs_error {
    RLFS_ERR_OK          = 0,    // No error
    RLFS_ERR_IO          = -5,   // Error during device operation
    RLFS_ERR_CORRUPT     = -84,  // Corrupted
    RLFS_ERR_NOENT       = -2,   // No directory entry
    RLFS_ERR_EXIST       = -17,  // Entry already exists
    RLFS_ERR_NOTDIR      = -20,  // Entry is not a dir
    RLFS_ERR_ISDIR       = -21,  // Entry is a dir
    RLFS_ERR_NOTEMPTY    = -39,  // Dir is not empty
    RLFS_ERR_BADF        = -9,   // Bad file number
    RLFS_ERR_FBIG        = -27,  // File too large
    RLFS_ERR_INVAL       = -22,  // Invalid parameter
    RLFS_ERR_NOSPC       = -28,  // No space left on device
    RLFS_ERR_NOMEM       = -12,  // No more memory available
    RLFS_ERR_NOATTR      = -61,  // No data/attr available
    RLFS_ERR_NAMETOOLONG = -36,  // File name too long
};

// File types
enum rlfs_type {
    // file types
    RLFS_TYPE_REG            = 0x001,
    RLFS_TYPE_DIR            = 0x002,

    // internally used types
    RLFS_TYPE_SPLICE         = 0x400,
    RLFS_TYPE_NAME           = 0x000,
    RLFS_TYPE_STRUCT         = 0x200,
    RLFS_TYPE_USERATTR       = 0x300,
    RLFS_TYPE_FROM           = 0x100,
    RLFS_TYPE_TAIL           = 0x600,
    RLFS_TYPE_GLOBALS        = 0x700,
    RLFS_TYPE_CRC            = 0x500,

    // internally used type specializations
    RLFS_TYPE_CREATE         = 0x401,
    RLFS_TYPE_DELETE         = 0x4ff,
    RLFS_TYPE_SUPERBLOCK     = 0x0ff,
    RLFS_TYPE_DIRSTRUCT      = 0x200,
    RLFS_TYPE_CTZSTRUCT      = 0x202,
    RLFS_TYPE_INLINESTRUCT   = 0x201,
    RLFS_TYPE_SOFTTAIL       = 0x600,
    RLFS_TYPE_HARDTAIL       = 0x601,
    RLFS_TYPE_MOVESTATE      = 0x7ff,
    RLFS_TYPE_CCRC           = 0x500,
    RLFS_TYPE_FCRC           = 0x5ff,

    // internal chip sources
    RLFS_FROM_NOOP           = 0x000,
    RLFS_FROM_MOVE           = 0x101,
    RLFS_FROM_USERATTRS      = 0x102,
};

// File open flags
enum rlfs_open_flags {
    // open flags
    RLFS_O_RDONLY = 1,         // Open a file as read only
#ifndef RLFS_READONLY
    RLFS_O_WRONLY = 2,         // Open a file as write only
    RLFS_O_RDWR   = 3,         // Open a file as read and write
    RLFS_O_CREAT  = 0x0100,    // Create a file if it does not exist
    RLFS_O_EXCL   = 0x0200,    // Fail if a file already exists
    RLFS_O_TRUNC  = 0x0400,    // Truncate the existing file to zero size
    RLFS_O_APPEND = 0x0800,    // Move to end of file on every write
#endif

    // internally used flags
#ifndef RLFS_READONLY
    RLFS_F_DIRTY   = 0x010000, // File does not match storage
    RLFS_F_WRITING = 0x020000, // File has been written since last flush
#endif
    RLFS_F_READING = 0x040000, // File has been read since last flush
#ifndef RLFS_READONLY
    RLFS_F_ERRED   = 0x080000, // An error occurred during write
#endif
    RLFS_F_INLINE  = 0x100000, // Currently inlined in directory entry
};

// File seek flags
enum rlfs_whence_flags {
    RLFS_SEEK_SET = 0,   // Seek relative to an absolute position
    RLFS_SEEK_CUR = 1,   // Seek relative to the current file position
    RLFS_SEEK_END = 2,   // Seek relative to the end of the file
};


// Configuration provided during initialization of the littlefs
struct rlfs_config {
    // Opaque user provided context that can be used to pass
    // information to the block device operations
    void *context;

    // Read a region in a block. Negative error codes are propagated
    // to the user.
    int (*read)(const struct rlfs_config *c, rlfs_block_t block,
            rlfs_off_t off, void *buffer, rlfs_size_t size);

    // Program a region in a block. The block must have previously
    // been erased. Negative error codes are propagated to the user.
    // May return RLFS_ERR_CORRUPT if the block should be considered bad.
    int (*prog)(const struct rlfs_config *c, rlfs_block_t block,
            rlfs_off_t off, const void *buffer, rlfs_size_t size);

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined. Negative error codes
    // are propagated to the user.
    // May return RLFS_ERR_CORRUPT if the block should be considered bad.
    int (*erase)(const struct rlfs_config *c, rlfs_block_t block);

    // Sync the state of the underlying block device. Negative error codes
    // are propagated to the user.
    int (*sync)(const struct rlfs_config *c);

#ifdef RLFS_THREADSAFE
    // Lock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*lock)(const struct rlfs_config *c);

    // Unlock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*unlock)(const struct rlfs_config *c);
#endif

    // Minimum size of a block read in bytes. All read operations will be a
    // multiple of this value.
    rlfs_size_t read_size;

    // Minimum size of a block program in bytes. All program operations will be
    // a multiple of this value.
    rlfs_size_t prog_size;

    // Size of an erasable block in bytes. This does not impact ram consumption
    // and may be larger than the physical erase size. However, non-inlined
    // files take up at minimum one block. Must be a multiple of the read and
    // program sizes.
    rlfs_size_t block_size;

    // Number of erasable blocks on the device. Defaults to block_count stored
    // on disk when zero.
    rlfs_size_t block_count;

    // Number of erase cycles before littlefs evicts metadata logs and moves
    // the metadata to another block. Suggested values are in the
    // range 100-1000, with large values having better performance at the cost
    // of less consistent wear distribution.
    //
    // Set to -1 to disable block-level wear-leveling.
    int32_t block_cycles;

    // Size of block caches in bytes. Each cache buffers a portion of a block in
    // RAM. The littlefs needs a read cache, a program cache, and one additional
    // cache per file. Larger caches can improve performance by storing more
    // data and reducing the number of disk accesses. Must be a multiple of the
    // read and program sizes, and a factor of the block size.
    rlfs_size_t cache_size;

    // Size of the lookahead buffer in bytes. A larger lookahead buffer
    // increases the number of blocks found during an allocation pass. The
    // lookahead buffer is stored as a compact bitmap, so each byte of RAM
    // can track 8 blocks.
    rlfs_size_t lookahead_size;

    // Threshold for metadata compaction during rlfs_fs_gc in bytes. Metadata
    // pairs that exceed this threshold will be compacted during rlfs_fs_gc.
    // Defaults to ~88% block_size when zero, though the default may change
    // in the future.
    //
    // Note this only affects rlfs_fs_gc. Normal compactions still only occur
    // when full.
    //
    // Set to -1 to disable metadata compaction during rlfs_fs_gc.
    rlfs_size_t compact_thresh;

    // Optional statically allocated read buffer. Must be cache_size.
    // By default rlfs_malloc is used to allocate this buffer.
    void *read_buffer;

    // Optional statically allocated program buffer. Must be cache_size.
    // By default rlfs_malloc is used to allocate this buffer.
    void *prog_buffer;

    // Optional statically allocated lookahead buffer. Must be lookahead_size.
    // By default rlfs_malloc is used to allocate this buffer.
    void *lookahead_buffer;

    // Optional upper limit on length of file names in bytes. No downside for
    // larger names except the size of the info struct which is controlled by
    // the RLFS_NAME_MAX define. Defaults to RLFS_NAME_MAX or name_max stored on
    // disk when zero.
    rlfs_size_t name_max;

    // Optional upper limit on files in bytes. No downside for larger files
    // but must be <= RLFS_FILE_MAX. Defaults to RLFS_FILE_MAX or file_max stored
    // on disk when zero.
    rlfs_size_t file_max;

    // Optional upper limit on custom attributes in bytes. No downside for
    // larger attributes size but must be <= RLFS_ATTR_MAX. Defaults to
    // RLFS_ATTR_MAX or attr_max stored on disk when zero.
    rlfs_size_t attr_max;

    // Optional upper limit on total space given to metadata pairs in bytes. On
    // devices with large blocks (e.g. 128kB) setting this to a low size (2-8kB)
    // can help bound the metadata compaction time. Must be <= block_size.
    // Defaults to block_size when zero.
    rlfs_size_t metadata_max;

    // Optional upper limit on inlined files in bytes. Inlined files live in
    // metadata and decrease storage requirements, but may be limited to
    // improve metadata-related performance. Must be <= cache_size, <=
    // attr_max, and <= block_size/8. Defaults to the largest possible
    // inline_max when zero.
    //
    // Set to -1 to disable inlined files.
    rlfs_size_t inline_max;

#ifdef RLFS_MULTIVERSION
    // On-disk version to use when writing in the form of 16-bit major version
    // + 16-bit minor version. This limiting metadata to what is supported by
    // older minor versions. Note that some features will be lost. Defaults to 
    // to the most recent minor version when zero.
    uint32_t disk_version;
#endif
};

// File info structure
struct rlfs_info {
    // Type of the file, either RLFS_TYPE_REG or RLFS_TYPE_DIR
    uint8_t type;

    // Size of the file, only valid for REG files. Limited to 32-bits.
    rlfs_size_t size;

    // Name of the file stored as a null-terminated string. Limited to
    // RLFS_NAME_MAX+1, which can be changed by redefining RLFS_NAME_MAX to
    // reduce RAM. RLFS_NAME_MAX is stored in superblock and must be
    // respected by other littlefs drivers.
    char name[RLFS_NAME_MAX+1];
};

// Filesystem info structure
struct rlfs_fsinfo {
    // On-disk version.
    uint32_t disk_version;

    // Size of a logical block in bytes.
    rlfs_size_t block_size;

    // Number of logical blocks in filesystem.
    rlfs_size_t block_count;

    // Upper limit on the length of file names in bytes.
    rlfs_size_t name_max;

    // Upper limit on the size of files in bytes.
    rlfs_size_t file_max;

    // Upper limit on the size of custom attributes in bytes.
    rlfs_size_t attr_max;
};

// Custom attribute structure, used to describe custom attributes
// committed atomically during file writes.
struct rlfs_attr {
    // 8-bit type of attribute, provided by user and used to
    // identify the attribute
    uint8_t type;

    // Pointer to buffer containing the attribute
    void *buffer;

    // Size of attribute in bytes, limited to RLFS_ATTR_MAX
    rlfs_size_t size;
};

// Optional configuration provided during rlfs_file_opencfg
struct rlfs_file_config {
    // Optional statically allocated file buffer. Must be cache_size.
    // By default rlfs_malloc is used to allocate this buffer.
    void *buffer;

    // Optional list of custom attributes related to the file. If the file
    // is opened with read access, these attributes will be read from disk
    // during the open call. If the file is opened with write access, the
    // attributes will be written to disk every file sync or close. This
    // write occurs atomically with update to the file's contents.
    //
    // Custom attributes are uniquely identified by an 8-bit type and limited
    // to RLFS_ATTR_MAX bytes. When read, if the stored attribute is smaller
    // than the buffer, it will be padded with zeros. If the stored attribute
    // is larger, then it will be silently truncated. If the attribute is not
    // found, it will be created implicitly.
    struct rlfs_attr *attrs;

    // Number of custom attributes in the list
    rlfs_size_t attr_count;
};


/// internal littlefs data structures ///
typedef struct rlfs_cache {
    rlfs_block_t block;
    rlfs_off_t off;
    rlfs_size_t size;
    uint8_t *buffer;
} rlfs_cache_t;

typedef struct rlfs_mdir {
    rlfs_block_t pair[2];
    uint32_t rev;
    rlfs_off_t off;
    uint32_t etag;
    uint16_t count;
    bool erased;
    bool split;
    rlfs_block_t tail[2];
} rlfs_mdir_t;

// littlefs directory type
typedef struct rlfs_dir {
    struct rlfs_dir *next;
    uint16_t id;
    uint8_t type;
    rlfs_mdir_t m;

    rlfs_off_t pos;
    rlfs_block_t head[2];
} rlfs_dir_t;

// littlefs file type
typedef struct rlfs_file {
    struct rlfs_file *next;
    uint16_t id;
    uint8_t type;
    rlfs_mdir_t m;

    struct rlfs_ctz {
        rlfs_block_t head;
        rlfs_size_t size;
    } ctz;

    uint32_t flags;
    rlfs_off_t pos;
    rlfs_block_t block;
    rlfs_off_t off;
    rlfs_cache_t cache;

    const struct rlfs_file_config *cfg;
} rlfs_file_t;

typedef struct rlfs_superblock {
    uint32_t version;
    rlfs_size_t block_size;
    rlfs_size_t block_count;
    rlfs_size_t name_max;
    rlfs_size_t file_max;
    rlfs_size_t attr_max;
} rlfs_superblock_t;

typedef struct rlfs_gstate {
    uint32_t tag;
    rlfs_block_t pair[2];
} rlfs_gstate_t;

// The littlefs filesystem type
typedef struct rlfs {
    rlfs_cache_t rcache;
    rlfs_cache_t pcache;

    rlfs_block_t root[2];
    struct rlfs_mlist {
        struct rlfs_mlist *next;
        uint16_t id;
        uint8_t type;
        rlfs_mdir_t m;
    } *mlist;
    uint32_t seed;

    rlfs_gstate_t gstate;
    rlfs_gstate_t gdisk;
    rlfs_gstate_t gdelta;

    struct rlfs_lookahead {
        rlfs_block_t start;
        rlfs_block_t size;
        rlfs_block_t next;
        rlfs_block_t ckpoint;
        uint8_t *buffer;
    } lookahead;

    const struct rlfs_config *cfg;
    rlfs_size_t block_count;
    rlfs_size_t name_max;
    rlfs_size_t file_max;
    rlfs_size_t attr_max;
    rlfs_size_t inline_max;

#ifdef RLFS_MIGRATE
    struct lfs1 *lfs1;
#endif
} rlfs_t;


/// Filesystem functions ///

#ifndef RLFS_READONLY
// Format a block device with the littlefs
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int rlfs_format(rlfs_t *lfs, const struct rlfs_config *config);
#endif

// Mounts a littlefs
//
// Requires a littlefs object and config struct. Multiple filesystems
// may be mounted simultaneously with multiple littlefs objects. Both
// lfs and config must be allocated while mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int rlfs_mount(rlfs_t *lfs, const struct rlfs_config *config);

// Unmounts a littlefs
//
// Does nothing besides releasing any allocated resources.
// Returns a negative error code on failure.
int rlfs_unmount(rlfs_t *lfs);

/// General operations ///

#ifndef RLFS_READONLY
// Removes a file or directory
//
// If removing a directory, the directory must be empty.
// Returns a negative error code on failure.
int rlfs_remove(rlfs_t *lfs, const char *path);
#endif

#ifndef RLFS_READONLY
// Rename or move a file or directory
//
// If the destination exists, it must match the source in type.
// If the destination is a directory, the directory must be empty.
//
// Returns a negative error code on failure.
int rlfs_rename(rlfs_t *lfs, const char *oldpath, const char *newpath);
#endif

// Find info about a file or directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a negative error code on failure.
int rlfs_stat(rlfs_t *lfs, const char *path, struct rlfs_info *info);

// Get a custom attribute
//
// Custom attributes are uniquely identified by an 8-bit type and limited
// to RLFS_ATTR_MAX bytes. When read, if the stored attribute is smaller than
// the buffer, it will be padded with zeros. If the stored attribute is larger,
// then it will be silently truncated. If no attribute is found, the error
// RLFS_ERR_NOATTR is returned and the buffer is filled with zeros.
//
// Returns the size of the attribute, or a negative error code on failure.
// Note, the returned size is the size of the attribute on disk, irrespective
// of the size of the buffer. This can be used to dynamically allocate a buffer
// or check for existence.
rlfs_ssize_t rlfs_getattr(rlfs_t *lfs, const char *path,
        uint8_t type, void *buffer, rlfs_size_t size);

#ifndef RLFS_READONLY
// Set custom attributes
//
// Custom attributes are uniquely identified by an 8-bit type and limited
// to RLFS_ATTR_MAX bytes. If an attribute is not found, it will be
// implicitly created.
//
// Returns a negative error code on failure.
int rlfs_setattr(rlfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, rlfs_size_t size);
#endif

#ifndef RLFS_READONLY
// Removes a custom attribute
//
// If an attribute is not found, nothing happens.
//
// Returns a negative error code on failure.
int rlfs_removeattr(rlfs_t *lfs, const char *path, uint8_t type);
#endif


/// File operations ///

#ifndef RLFS_NO_MALLOC
// Open a file
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum rlfs_open_flags that are bitwise-ored together.
//
// Returns a negative error code on failure.
int rlfs_file_open(rlfs_t *lfs, rlfs_file_t *file,
        const char *path, int flags);

// if RLFS_NO_MALLOC is defined, rlfs_file_open() will fail with RLFS_ERR_NOMEM
// thus use rlfs_file_opencfg() with config.buffer set.
#endif

// Open a file with extra configuration
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum rlfs_open_flags that are bitwise-ored together.
//
// The config struct provides additional config options per file as described
// above. The config struct must remain allocated while the file is open, and
// the config struct must be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int rlfs_file_opencfg(rlfs_t *lfs, rlfs_file_t *file,
        const char *path, int flags,
        const struct rlfs_file_config *config);

// Close a file
//
// Any pending writes are written out to storage as though
// sync had been called and releases any allocated resources.
//
// Returns a negative error code on failure.
int rlfs_file_close(rlfs_t *lfs, rlfs_file_t *file);

// Synchronize a file on storage
//
// Any pending writes are written out to storage.
// Returns a negative error code on failure.
int rlfs_file_sync(rlfs_t *lfs, rlfs_file_t *file);

// Read data from file
//
// Takes a buffer and size indicating where to store the read data.
// Returns the number of bytes read, or a negative error code on failure.
rlfs_ssize_t rlfs_file_read(rlfs_t *lfs, rlfs_file_t *file,
        void *buffer, rlfs_size_t size);

#ifndef RLFS_READONLY
// Write data to file
//
// Takes a buffer and size indicating the data to write. The file will not
// actually be updated on the storage until either sync or close is called.
//
// Returns the number of bytes written, or a negative error code on failure.
rlfs_ssize_t rlfs_file_write(rlfs_t *lfs, rlfs_file_t *file,
        const void *buffer, rlfs_size_t size);
#endif

// Change the position of the file
//
// The change in position is determined by the offset and whence flag.
// Returns the new position of the file, or a negative error code on failure.
rlfs_soff_t rlfs_file_seek(rlfs_t *lfs, rlfs_file_t *file,
        rlfs_soff_t off, int whence);

#ifndef RLFS_READONLY
// Truncates the size of the file to the specified size
//
// Returns a negative error code on failure.
int rlfs_file_truncate(rlfs_t *lfs, rlfs_file_t *file, rlfs_off_t size);
#endif

// Return the position of the file
//
// Equivalent to rlfs_file_seek(lfs, file, 0, RLFS_SEEK_CUR)
// Returns the position of the file, or a negative error code on failure.
rlfs_soff_t rlfs_file_tell(rlfs_t *lfs, rlfs_file_t *file);

// Change the position of the file to the beginning of the file
//
// Equivalent to rlfs_file_seek(lfs, file, 0, RLFS_SEEK_SET)
// Returns a negative error code on failure.
int rlfs_file_rewind(rlfs_t *lfs, rlfs_file_t *file);

// Return the size of the file
//
// Similar to rlfs_file_seek(lfs, file, 0, RLFS_SEEK_END)
// Returns the size of the file, or a negative error code on failure.
rlfs_soff_t rlfs_file_size(rlfs_t *lfs, rlfs_file_t *file);


/// Directory operations ///

#ifndef RLFS_READONLY
// Create a directory
//
// Returns a negative error code on failure.
int rlfs_mkdir(rlfs_t *lfs, const char *path);
#endif

// Open a directory
//
// Once open a directory can be used with read to iterate over files.
// Returns a negative error code on failure.
int rlfs_dir_open(rlfs_t *lfs, rlfs_dir_t *dir, const char *path);

// Close a directory
//
// Releases any allocated resources.
// Returns a negative error code on failure.
int rlfs_dir_close(rlfs_t *lfs, rlfs_dir_t *dir);

// Read an entry in the directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a positive value on success, 0 at the end of directory,
// or a negative error code on failure.
int rlfs_dir_read(rlfs_t *lfs, rlfs_dir_t *dir, struct rlfs_info *info);

// Change the position of the directory
//
// The new off must be a value previous returned from tell and specifies
// an absolute offset in the directory seek.
//
// Returns a negative error code on failure.
int rlfs_dir_seek(rlfs_t *lfs, rlfs_dir_t *dir, rlfs_off_t off);

// Return the position of the directory
//
// The returned offset is only meant to be consumed by seek and may not make
// sense, but does indicate the current position in the directory iteration.
//
// Returns the position of the directory, or a negative error code on failure.
rlfs_soff_t rlfs_dir_tell(rlfs_t *lfs, rlfs_dir_t *dir);

// Change the position of the directory to the beginning of the directory
//
// Returns a negative error code on failure.
int rlfs_dir_rewind(rlfs_t *lfs, rlfs_dir_t *dir);


/// Filesystem-level filesystem operations

// Find on-disk info about the filesystem
//
// Fills out the fsinfo structure based on the filesystem found on-disk.
// Returns a negative error code on failure.
int rlfs_fs_stat(rlfs_t *lfs, struct rlfs_fsinfo *fsinfo);

// Finds the current size of the filesystem
//
// Note: Result is best effort. If files share COW structures, the returned
// size may be larger than the filesystem actually is.
//
// Returns the number of allocated blocks, or a negative error code on failure.
rlfs_ssize_t rlfs_fs_size(rlfs_t *lfs);

// Traverse through all blocks in use by the filesystem
//
// The provided callback will be called with each block address that is
// currently in use by the filesystem. This can be used to determine which
// blocks are in use or how much of the storage is available.
//
// Returns a negative error code on failure.
int rlfs_fs_traverse(rlfs_t *lfs, int (*cb)(void*, rlfs_block_t), void *data);

#ifndef RLFS_READONLY
// Attempt to make the filesystem consistent and ready for writing
//
// Calling this function is not required, consistency will be implicitly
// enforced on the first operation that writes to the filesystem, but this
// function allows the work to be performed earlier and without other
// filesystem changes.
//
// Returns a negative error code on failure.
int rlfs_fs_mkconsistent(rlfs_t *lfs);
#endif

#ifndef RLFS_READONLY
// Attempt any janitorial work
//
// This currently:
// 1. Calls mkconsistent if not already consistent
// 2. Compacts metadata > compact_thresh
// 3. Populates the block allocator
//
// Though additional janitorial work may be added in the future.
//
// Calling this function is not required, but may allow the offloading of
// expensive janitorial work to a less time-critical code path.
//
// Returns a negative error code on failure. Accomplishing nothing is not
// an error.
int rlfs_fs_gc(rlfs_t *lfs);
#endif

#ifndef RLFS_READONLY
// Grows the filesystem to a new size, updating the superblock with the new
// block count.
//
// If RLFS_SHRINKNONRELOCATING is defined, this function will also accept
// block_counts smaller than the current configuration, after checking
// that none of the blocks that are being removed are in use.
// Note that littlefs's pseudorandom block allocation means that
// this is very unlikely to work in the general case.
//
// Returns a negative error code on failure.
int rlfs_fs_grow(rlfs_t *lfs, rlfs_size_t block_count);
#endif

#ifndef RLFS_READONLY
#ifdef RLFS_MIGRATE
// Attempts to migrate a previous version of littlefs
//
// Behaves similarly to the rlfs_format function. Attempts to mount
// the previous version of littlefs and update the filesystem so it can be
// mounted with the current version of littlefs.
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int rlfs_migrate(rlfs_t *lfs, const struct rlfs_config *cfg);
#endif
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
