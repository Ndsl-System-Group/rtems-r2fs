#include "rtfs_test.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>

#include "cache/block_buffer.h"
#include "cache/node_block_cache.h"
#include "fs/dir_inode/dir_inode.h"


static RtfsRuntimeInodeView makeChildView(rtfs_ino ino, rtfs_ino parent_ino, uint8_t file_type)
{
    RtfsRuntimeInodeView view;

    rtfsRuntimeInodeViewInit(&view, ino, parent_ino, file_type);
    return view;
}

static void assertLookupResult(
    const RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino expected_ino,
    rtfs_ino expected_parent_ino,
    uint8_t expected_file_type
)
{
    RtfsDirLookupResult result;

    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeLookup(dir_inode, name, strlen(name), &result)
    );
    TEST_ASSERT_EQUAL(expected_ino, result.inode_view.ino);
    TEST_ASSERT_EQUAL(expected_parent_ino, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(expected_file_type, result.inode_view.file_type);
}

static struct dirent *direntAt(void *buffer, size_t index)
{
    return (struct dirent *)((char *)buffer + index * sizeof(struct dirent));
}

static RtfsDirInode *mustLoadDirInode(RtfsDirInodeCache *cache, rtfs_ino ino)
{
    RtfsDirInode *dir_inode = NULL;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, ino, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    return dir_inode;
}

static void setBitmapBit(uint8_t *bitmap, size_t bit_index)
{
    bitmap[bit_index / 8] |= (uint8_t)(1u << (bit_index % 8));
}

static void writeInlineDentryName(
    struct RtfsInlineDentry *inline_dentry,
    size_t index,
    const char *name
)
{
    size_t name_len = strlen(name);
    size_t slot_count = GET_DENTRY_SLOTS(name_len);
    size_t slot;
    size_t offset = 0;

    for (slot = 0; slot < slot_count; ++slot) {
        size_t copy_len = RTFS_SLOT_LEN;
        if (offset + copy_len > name_len) {
            copy_len = name_len - offset;
        }

        memcpy(inline_dentry->filename[index + slot], name + offset, copy_len);
        offset += copy_len;
    }
}

static void addInlineDentry(
    struct RtfsInlineDentry *inline_dentry,
    size_t index,
    rtfs_ino ino,
    uint8_t file_type,
    const char *name
)
{
    size_t slot_count = GET_DENTRY_SLOTS(strlen(name));
    size_t slot;

    inline_dentry->dentry[index].ino = ino;
    inline_dentry->dentry[index].name_len = strlen(name);
    inline_dentry->dentry[index].file_type = file_type;
    for (slot = 0; slot < slot_count; ++slot) {
        setBitmapBit(inline_dentry->dentry_bitmap, index + slot);
    }
    writeInlineDentryName(inline_dentry, index, name);
}

static void prepareInlineDirectoryNode(
    BlockBuffer *buffer,
    rtfs_ino ino,
    rtfs_ino parent_ino
)
{
    struct RtfsNode *node = (struct RtfsNode *)blockBufferGetPtr(buffer);
    struct RtfsInlineDentry *inline_dentry;

    memset(node, 0, sizeof(*node));
    node->i.i_inline = RTFS_INLINE_DENTRY;
    node->i.i_type = RTFS_FT_DIR;
    node->i.i_pino = parent_ino;
    node->i.i_size = BLOCK_BUFFER_SIZE;
    node->i.i_dentry_num = 2;
    node->i.i_mtime = 12345;
    node->i.i_current_depth = 1;
    node->footer.nid = ino;
    node->footer.ino = ino;

    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;
    addInlineDentry(inline_dentry, 0, 3001, RTFS_FT_REG_FILE, "alpha");
    addInlineDentry(inline_dentry, 1, 3002, RTFS_FT_DIR, "dirchild");
}


RTFS_TEST(DirInodeLoadAndPut_ShouldCreateEmptyDirectoryObject)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 100);

    assertLookupResult(dir_inode, ".", 100, 100, RTFS_FT_DIR);
    assertLookupResult(dir_inode, "..", 100, 100, RTFS_FT_DIR);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLoad_WithCacheMiss_ShouldReturnENOENT)
{
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode = NULL;

    nodeBlockCacheInit(&node_cache, NULL, 8);
    cache = rtfsDirInodeCacheCreate(&node_cache);

    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeBuild(cache, 404, &dir_inode));
    TEST_ASSERT_NULL(dir_inode);

    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheDestroy(&node_cache);
}

RTFS_TEST(DirInodeBuild_WithInlineDirectoryInNodeCache_ShouldLoadMetadataAndEntries)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct dirent entries[5];
    off_t offset = 0;
    ssize_t bytes_read;

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2000, 1999);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2000, INVALID_NID, 77);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2000, &dir_inode));

    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_NOT_NULL(dir_inode);

    assertLookupResult(dir_inode, ".", 2000, 1999, RTFS_FT_DIR);
    assertLookupResult(dir_inode, "..", 1999, 1999, RTFS_FT_DIR);
    assertLookupResult(dir_inode, "alpha", 3001, 2000, RTFS_FT_REG_FILE);
    assertLookupResult(dir_inode, "dirchild", 3002, 2000, RTFS_FT_DIR);

    memset(entries, 0, sizeof(entries));
    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));
    TEST_ASSERT_EQUAL(4 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL_STRING("alpha", direntAt(entries, 2)->d_name);
    TEST_ASSERT_EQUAL_STRING("dirchild", direntAt(entries, 3)->d_name);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeRemoveEntry_WhenInlineBacked_ShouldSyncInlineDentryAndMetadata)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    struct RtfsInlineDentry *inline_dentry;
    RtfsDirLookupResult result;

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2100, 2099);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2100, INVALID_NID, 88);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2100, &dir_inode));

    node = (struct RtfsNode *)blockBufferGetPtr(nodeBlockCacheEntryGetNodeBuffer(handle.entry));
    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "alpha"));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "alpha", 5, &result));
    TEST_ASSERT_FALSE((inline_dentry->dentry_bitmap[0] & 0x01u) != 0);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)node->i.i_dentry_num);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeAddEntry_WhenInlineBacked_ShouldSyncInlineDentryAndMetadata)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    struct RtfsInlineDentry *inline_dentry;
    RtfsRuntimeInodeView child_view = makeChildView(3100, 0, RTFS_FT_REG_FILE);

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2200, 2199);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2200, INVALID_NID, 99);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2200, &dir_inode));

    node = (struct RtfsNode *)blockBufferGetPtr(nodeBlockCacheEntryGetNodeBuffer(handle.entry));
    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "new", &child_view));
    assertLookupResult(dir_inode, "new", 3100, 2200, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)node->i.i_dentry_num);
    TEST_ASSERT_TRUE((inline_dentry->dentry_bitmap[0] & 0x04u) != 0);
    TEST_ASSERT_EQUAL(3100u, inline_dentry->dentry[2].ino);
    TEST_ASSERT_EQUAL(3u, inline_dentry->dentry[2].name_len);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, inline_dentry->dentry[2].file_type);
    TEST_ASSERT_EQUAL_MEMORY("new", inline_dentry->filename[2], 3);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeAddEntry_WhenInlineBackedWithMultiSlotName_ShouldSyncBitmapAndFilenameAcrossSlots)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    struct RtfsInlineDentry *inline_dentry;
    RtfsRuntimeInodeView child_view = makeChildView(3200, 0, RTFS_FT_REG_FILE);
    const char *name = "abcdefghijklmnopqrst";

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2210, 2209);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2210, INVALID_NID, 100);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2210, &dir_inode));

    node = (struct RtfsNode *)blockBufferGetPtr(nodeBlockCacheEntryGetNodeBuffer(handle.entry));
    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view));
    assertLookupResult(dir_inode, name, 3200, 2210, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)node->i.i_dentry_num);
    TEST_ASSERT_TRUE((inline_dentry->dentry_bitmap[0] & 0x1Cu) == 0x1Cu);
    TEST_ASSERT_EQUAL(3200u, inline_dentry->dentry[2].ino);
    TEST_ASSERT_EQUAL(strlen(name), inline_dentry->dentry[2].name_len);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, inline_dentry->dentry[2].file_type);
    TEST_ASSERT_EQUAL_MEMORY("abcdefgh", inline_dentry->filename[2], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY("ijklmnop", inline_dentry->filename[3], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY("qrst", inline_dentry->filename[4], 4);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeRemoveEntry_WhenInlineMultiSlotEntryIsRemoved_ShouldAllowSlotReuse)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    struct RtfsInlineDentry *inline_dentry;
    RtfsRuntimeInodeView long_child = makeChildView(3201, 0, RTFS_FT_REG_FILE);
    RtfsRuntimeInodeView short_child = makeChildView(3202, 0, RTFS_FT_DIR);
    const char *long_name = "abcdefghijklmnopqrst";
    RtfsDirLookupResult result;

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2211, 2210);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2211, INVALID_NID, 101);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2211, &dir_inode));

    node = (struct RtfsNode *)blockBufferGetPtr(nodeBlockCacheEntryGetNodeBuffer(handle.entry));
    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, long_name, &long_child));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, long_name));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, long_name, strlen(long_name), &result));
    TEST_ASSERT_FALSE((inline_dentry->dentry_bitmap[0] & 0x04u) != 0);
    TEST_ASSERT_FALSE((inline_dentry->dentry_bitmap[0] & 0x08u) != 0);
    TEST_ASSERT_FALSE((inline_dentry->dentry_bitmap[0] & 0x10u) != 0);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "beta", &short_child));
    assertLookupResult(dir_inode, "beta", 3202, 2211, RTFS_FT_DIR);
    TEST_ASSERT_TRUE((inline_dentry->dentry_bitmap[0] & 0x04u) != 0);
    TEST_ASSERT_EQUAL(3202u, inline_dentry->dentry[2].ino);
    TEST_ASSERT_EQUAL(4u, inline_dentry->dentry[2].name_len);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, inline_dentry->dentry[2].file_type);
    TEST_ASSERT_EQUAL_MEMORY("beta", inline_dentry->filename[2], 4);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeRemoveEntry_WhenInlineMultiSlotEntryIsRemoved_ShouldClearAllFilenameSlots)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    struct RtfsInlineDentry *inline_dentry;
    RtfsRuntimeInodeView child_view = makeChildView(3203, 0, RTFS_FT_REG_FILE);
    const char *name = "abcdefghijklmnopqrst";
    uint8_t zero_slot[RTFS_SLOT_LEN] = {0};

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2212, 2211);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2212, INVALID_NID, 102);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2212, &dir_inode));

    node = (struct RtfsNode *)blockBufferGetPtr(nodeBlockCacheEntryGetNodeBuffer(handle.entry));
    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, name));
    TEST_ASSERT_EQUAL_MEMORY(zero_slot, inline_dentry->filename[2], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY(zero_slot, inline_dentry->filename[3], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY(zero_slot, inline_dentry->filename[4], RTFS_SLOT_LEN);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeAddEntry_WhenInlineNameLengthExactlyTwoSlots_ShouldUseExactlyTwoSlots)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    struct RtfsInlineDentry *inline_dentry;
    RtfsRuntimeInodeView child_view = makeChildView(3204, 0, RTFS_FT_DIR);
    const char *name = "abcdefghijklmnop";

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2213, 2212);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2213, INVALID_NID, 103);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2213, &dir_inode));

    node = (struct RtfsNode *)blockBufferGetPtr(nodeBlockCacheEntryGetNodeBuffer(handle.entry));
    inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view));
    assertLookupResult(dir_inode, name, 3204, 2213, RTFS_FT_DIR);
    TEST_ASSERT_TRUE((inline_dentry->dentry_bitmap[0] & 0x0Cu) == 0x0Cu);
    TEST_ASSERT_FALSE((inline_dentry->dentry_bitmap[0] & 0x10u) != 0);
    TEST_ASSERT_EQUAL(16u, inline_dentry->dentry[2].name_len);
    TEST_ASSERT_EQUAL_MEMORY("abcdefgh", inline_dentry->filename[2], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY("ijklmnop", inline_dentry->filename[3], RTFS_SLOT_LEN);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodePut_WhenNull_ShouldBeSafe)
{
    rtfsDirInodePut(NULL);
    TEST_PASS();
}

RTFS_TEST(DirInodeLoad_WhenOutParamIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeBuild(NULL, 1, NULL));
}

RTFS_TEST(DirInodeLookup_WhenEntryExists_ShouldReturnChildRuntimeView)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 200);
    RtfsRuntimeInodeView child_view = makeChildView(300, 999, RTFS_FT_REG_FILE);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "file.txt", &child_view));

    assertLookupResult(dir_inode, "file.txt", 300, 200, RTFS_FT_REG_FILE);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLookup_WhenNameMissing_ShouldReturnENOENT)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 88);
    RtfsDirLookupResult result;

    TEST_ASSERT_EQUAL(
        ENOENT,
        rtfsDirInodeLookup(dir_inode, "missing", strlen("missing"), &result)
    );

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLookup_WhenNameLengthDoesNotMatchStoredName_ShouldReturnENOENT)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 201);
    RtfsRuntimeInodeView child_view = makeChildView(301, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "hello", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "hello", 4, &result));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLookup_WhenArgumentsAreInvalid_ShouldReject)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 55);
    RtfsDirLookupResult result;

    TEST_ASSERT_EQUAL(ENOTDIR, rtfsDirInodeLookup(NULL, "a", 1, &result));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeLookup(dir_inode, NULL, 1, &result));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeLookup(dir_inode, "a", 1, NULL));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenDirectoryIsEmpty_ShouldReturnDotAndDotDot)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 42);
    struct dirent entries[4];
    off_t offset = 0;
    ssize_t bytes_read;

    memset(entries, 0, sizeof(entries));

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(2 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL(2 * (off_t)sizeof(struct dirent), offset);
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL(42, direntAt(entries, 0)->d_ino);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL(42, direntAt(entries, 1)->d_ino);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenDirectoryHasChildren_ShouldReturnAllEntriesInSequence)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 500);
    RtfsRuntimeInodeView file_view = makeChildView(501, 0, RTFS_FT_REG_FILE);
    RtfsRuntimeInodeView subdir_view = makeChildView(502, 0, RTFS_FT_DIR);
    struct dirent entries[6];
    off_t offset = 0;
    ssize_t bytes_read;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "alpha", &file_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "beta", &subdir_view));
    memset(entries, 0, sizeof(entries));

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(4 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL(4 * (off_t)sizeof(struct dirent), offset);
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL_STRING("alpha", direntAt(entries, 2)->d_name);
    TEST_ASSERT_EQUAL(501, direntAt(entries, 2)->d_ino);
    TEST_ASSERT_EQUAL_STRING("beta", direntAt(entries, 3)->d_name);
    TEST_ASSERT_EQUAL(502, direntAt(entries, 3)->d_ino);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenReadingWithSmallBuffer_ShouldAdvanceOffsetIncrementally)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 700);
    RtfsRuntimeInodeView child_view = makeChildView(701, 0, RTFS_FT_REG_FILE);
    struct dirent entry;
    off_t offset = 0;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "gamma", &child_view));

    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL(
        (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, &entry, sizeof(entry))
    );
    TEST_ASSERT_EQUAL_STRING(".", entry.d_name);
    TEST_ASSERT_EQUAL((off_t)sizeof(struct dirent), offset);

    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL(
        (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, &entry, sizeof(entry))
    );
    TEST_ASSERT_EQUAL_STRING("..", entry.d_name);
    TEST_ASSERT_EQUAL(2 * (off_t)sizeof(struct dirent), offset);

    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL(
        (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, &entry, sizeof(entry))
    );
    TEST_ASSERT_EQUAL_STRING("gamma", entry.d_name);
    TEST_ASSERT_EQUAL(701, entry.d_ino);
    TEST_ASSERT_EQUAL(3 * (off_t)sizeof(struct dirent), offset);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenBufferIsSmallerThanOneDirent_ShouldReturnZeroAndKeepOffset)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 710);
    char buffer[sizeof(struct dirent) - 1];
    off_t offset = 0;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeReadEntries(dir_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(0, offset);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenOffsetIsAtEnd_ShouldReturnZeroAndKeepOffset)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 720);
    struct dirent entries[4];
    off_t offset = 0;
    ssize_t bytes_read;
    off_t end_offset;

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));
    TEST_ASSERT_EQUAL(2 * (ssize_t)sizeof(struct dirent), bytes_read);
    end_offset = offset;

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));
    TEST_ASSERT_EQUAL(0, bytes_read);
    TEST_ASSERT_EQUAL(end_offset, offset);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAddEntry_WhenDuplicateOrInvalid_ShouldReject)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 900);
    RtfsRuntimeInodeView child_view = makeChildView(901, 0, RTFS_FT_REG_FILE);
    char long_name[RTFS_NAME_LEN + 2];

    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "dup", &child_view));
    TEST_ASSERT_EQUAL(EEXIST, rtfsDirInodeAddEntry(dir_inode, "dup", &child_view));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeAddEntry(dir_inode, long_name, &child_view));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeAddEntry(dir_inode, "", &child_view));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeAddEntry(NULL, "x", &child_view));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeAddEntry(dir_inode, NULL, &child_view));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeAddEntry(dir_inode, "x", NULL));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenEntryExists_ShouldRemoveItFromLookupAndReadDir)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1000);
    RtfsRuntimeInodeView first_view = makeChildView(1001, 0, RTFS_FT_REG_FILE);
    RtfsRuntimeInodeView second_view = makeChildView(1002, 0, RTFS_FT_DIR);
    struct dirent entries[5];
    off_t offset = 0;
    RtfsDirLookupResult result;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "first", &first_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "second", &second_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "first"));
    TEST_ASSERT_EQUAL(
        ENOENT,
        rtfsDirInodeLookup(dir_inode, "first", strlen("first"), &result)
    );
    assertLookupResult(dir_inode, "second", 1002, 1000, RTFS_FT_DIR);

    memset(entries, 0, sizeof(entries));
    TEST_ASSERT_EQUAL(
        3 * (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries))
    );
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL_STRING("second", direntAt(entries, 2)->d_name);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenEntryDoesNotExistOrArgumentsInvalid_ShouldReject)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1100);
    char long_name[RTFS_NAME_LEN + 2];

    memset(long_name, 'b', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeRemoveEntry(dir_inode, "missing"));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeRemoveEntry(dir_inode, ""));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeRemoveEntry(dir_inode, long_name));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeRemoveEntry(NULL, "x"));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeRemoveEntry(dir_inode, NULL));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenRemovingSameEntryTwice_ShouldReturnENOENTSecondTime)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1110);
    RtfsRuntimeInodeView child_view = makeChildView(1111, 0, RTFS_FT_REG_FILE);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "once", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "once"));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeRemoveEntry(dir_inode, "once"));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenArgumentsInvalid_ShouldReturnMinusOneAndSetErrno)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1200);
    struct dirent entry;
    off_t offset = 0;

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsDirInodeReadEntries(NULL, &offset, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(ENOTDIR, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsDirInodeReadEntries(dir_inode, NULL, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsDirInodeReadEntries(dir_inode, &offset, NULL, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAppendDentryBlock_WhenRegularEntriesExist_ShouldImportThem)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1300);
    struct RtfsDentryBlock block;
    RtfsDirLookupResult result;
    struct dirent entries[4];
    off_t offset = 0;
    ssize_t bytes_read;

    memset(&block, 0, sizeof(block));

    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1301;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    block.dentry_bitmap[0] |= (1u << 1);
    block.dentry[1].ino = 1302;
    block.dentry[1].name_len = 1;
    block.dentry[1].file_type = RTFS_FT_DIR;
    memcpy(block.filename[1], ".", 1);

    block.dentry_bitmap[0] |= (1u << 2);
    block.dentry[2].ino = 1303;
    block.dentry[2].name_len = 2;
    block.dentry[2].file_type = RTFS_FT_DIR;
    memcpy(block.filename[2], "..", 2);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "alpha", 5, &result));
    TEST_ASSERT_EQUAL(1301, result.inode_view.ino);
    TEST_ASSERT_EQUAL(1300, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, result.inode_view.file_type);

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));
    TEST_ASSERT_EQUAL((ssize_t)(3 * sizeof(struct dirent)), bytes_read);
    TEST_ASSERT_EQUAL_STRING(".", entries[0].d_name);
    TEST_ASSERT_EQUAL_STRING("..", entries[1].d_name);
    TEST_ASSERT_EQUAL_STRING("alpha", entries[2].d_name);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAddEntry_WhenRegularBlockBacked_ShouldSyncRegularBlockAndMetadata)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1320);
    struct RtfsDentryBlock block;
    RtfsRuntimeInodeView child_view = makeChildView(1322, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1321;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "beta", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));
    TEST_ASSERT_EQUAL(1322u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(1320u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAddEntry_WhenRegularBlockBackedWithMultiSlotName_ShouldSyncBitmapAndFilenameAcrossSlots)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1323);
    struct RtfsDentryBlock block;
    RtfsRuntimeInodeView child_view = makeChildView(1324, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;
    const char *long_name = "abcdefghijklmnopqrst";

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1321;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, long_name, &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, long_name, strlen(long_name), &result));
    TEST_ASSERT_EQUAL(1324u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(1323u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenRegularBlockBacked_ShouldSyncRegularBlockAndMetadata)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1310);
    struct RtfsDentryBlock block;
    RtfsDirLookupResult result;

    memset(&block, 0, sizeof(block));

    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1311;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "alpha"));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "alpha", 5, &result));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenRegularMultiSlotEntryIsRemoved_ShouldAllowSlotReuse)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1313);
    struct RtfsDentryBlock block;
    RtfsRuntimeInodeView first_view = makeChildView(1314, 0, RTFS_FT_REG_FILE);
    RtfsRuntimeInodeView second_view = makeChildView(1315, 0, RTFS_FT_DIR);
    RtfsDirLookupResult result;
    const char *long_name = "abcdefghijklmnopqrst";

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1311;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, long_name, &first_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, long_name));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, long_name, strlen(long_name), &result));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "beta", &second_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));
    TEST_ASSERT_EQUAL(1315u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(1313u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenRegularMultiSlotEntryIsRemoved_ShouldClearAllFilenameSlots)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1316);
    struct RtfsDentryBlock block;
    RtfsRuntimeInodeView child_view = makeChildView(1317, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;
    const char *long_name = "abcdefghijklmnopqrst";

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1311;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, long_name, &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, long_name));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, long_name, strlen(long_name), &result));
    TEST_ASSERT_EQUAL_MEMORY((uint8_t[RTFS_SLOT_LEN]){0}, block.filename[1], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY((uint8_t[RTFS_SLOT_LEN]){0}, block.filename[2], RTFS_SLOT_LEN);
    TEST_ASSERT_EQUAL_MEMORY((uint8_t[RTFS_SLOT_LEN]){0}, block.filename[3], RTFS_SLOT_LEN);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAddEntry_WhenRegularNameLengthExactlyTwoSlots_ShouldUseExactlyTwoSlots)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1318);
    struct RtfsDentryBlock block;
    RtfsRuntimeInodeView child_view = makeChildView(1319, 0, RTFS_FT_DIR);
    RtfsDirLookupResult result;
    const char *name = "abcdefghijklmnop";

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1311;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, name, strlen(name), &result));
    TEST_ASSERT_EQUAL(1319u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(1318u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAddEntry_WhenInlineNameIsNearRtfsNameLen_ShouldRemainLookupable)
{
    BlockBuffer buffer;
    NodeBlockCache node_cache;
    RtfsDirInodeCache *cache;
    RtfsDirInode *dir_inode;
    NodeBlockCacheEntryHandle handle;
    RtfsRuntimeInodeView child_view = makeChildView(3205, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;
    char name[RTFS_NAME_LEN];
    size_t i;

    for (i = 0; i < RTFS_NAME_LEN - 1; ++i) {
        name[i] = (char)('a' + (i % 26));
    }
    name[RTFS_NAME_LEN - 1] = '\0';

    blockBufferInit(&buffer);
    prepareInlineDirectoryNode(&buffer, 2214, 2213);

    nodeBlockCacheInit(&node_cache, NULL, 8);
    handle = nodeBlockCacheAdd(&node_cache, &buffer, 2214, INVALID_NID, 104);
    cache = rtfsDirInodeCacheCreate(&node_cache);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(cache, 2214, &dir_inode));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, name, strlen(name), &result));
    TEST_ASSERT_EQUAL(3205u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(2214u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
    rtfsDirInodeCacheDestroy(cache);
    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&node_cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(DirInodeAddEntry_WhenRegularNameIsNearRtfsNameLen_ShouldRemainLookupable)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1320);
    struct RtfsDentryBlock block;
    RtfsRuntimeInodeView child_view = makeChildView(1321, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;
    char name[RTFS_NAME_LEN];
    size_t i;

    for (i = 0; i < RTFS_NAME_LEN - 1; ++i) {
        name[i] = (char)('a' + (i % 26));
    }
    name[RTFS_NAME_LEN - 1] = '\0';

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1311;
    block.dentry[0].name_len = 5;
    block.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(block.filename[0], "alpha", 5);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, name, strlen(name), &result));
    TEST_ASSERT_EQUAL(1321u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(1320u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLoadProgress_WhenBuiltWithoutCache_ShouldStartUnloaded)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1400);

    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rtfsDirInodeGetTotalBlockCount(dir_inode));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLoadProgress_WhenRegularBlockIsAppended_ShouldRemainUnloadedWithoutMetadata)
{
    RtfsDirInode *dir_inode = mustLoadDirInode(NULL, 1500);
    struct RtfsDentryBlock block;

    memset(&block, 0, sizeof(block));
    block.dentry_bitmap[0] |= (1u << 0);
    block.dentry[0].ino = 1501;
    block.dentry[0].name_len = 4;
    block.dentry[0].file_type = RTFS_FT_REG_FILE;
    memcpy(block.filename[0], "beta", 4);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlock(dir_inode, &block));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rtfsDirInodeGetTotalBlockCount(dir_inode));

    rtfsDirInodePut(dir_inode);
}
