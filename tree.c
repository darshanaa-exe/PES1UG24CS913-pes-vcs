// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "pes.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int index_load(Index *index) __attribute__((weak));

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = (size_t)(space - ptr);
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = (size_t)(null_byte - ptr);
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += (size_t)written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index;
    Tree root;

    if (!id_out) return -1;
    if (!index_load) return -1;
    if (index_load(&index) != 0) return -1;

    root.count = 0;

    for (int i = 0; i < index.count; i++) {
        const char *path = index.entries[i].path;
        const char *slash = strchr(path, '/');

        if (!slash) {
            if (root.count >= MAX_TREE_ENTRIES) return -1;

            TreeEntry *entry = &root.entries[root.count++];
            entry->mode = index.entries[i].mode;
            entry->hash = index.entries[i].hash;
            strncpy(entry->name, path, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
        } else {
            char dirname[256];
            size_t dirlen = (size_t)(slash - path);
            if (dirlen >= sizeof(dirname)) return -1;

            memcpy(dirname, path, dirlen);
            dirname[dirlen] = '\0';

            int exists = 0;
            for (int j = 0; j < root.count; j++) {
                if (strcmp(root.entries[j].name, dirname) == 0 &&
                    root.entries[j].mode == MODE_DIR) {
                    exists = 1;
                    break;
                }
            }

            if (!exists) {
                if (root.count >= MAX_TREE_ENTRIES) return -1;

                Tree subtree;
                subtree.count = 0;

                for (int k = 0; k < index.count; k++) {
                    const char *subpath = index.entries[k].path;

                    if (strncmp(subpath, dirname, dirlen) == 0 &&
                        subpath[dirlen] == '/') {

                        const char *rest = subpath + dirlen + 1;

                        if (strchr(rest, '/')) continue;

                        if (subtree.count >= MAX_TREE_ENTRIES) return -1;

                        TreeEntry *subentry = &subtree.entries[subtree.count++];
                        subentry->mode = index.entries[k].mode;
                        subentry->hash = index.entries[k].hash;
                        strncpy(subentry->name, rest, sizeof(subentry->name) - 1);
                        subentry->name[sizeof(subentry->name) - 1] = '\0';
                    }
                }

                void *subdata = NULL;
                size_t sublen = 0;
                ObjectID subtree_id;

                if (tree_serialize(&subtree, &subdata, &sublen) != 0) return -1;
                if (object_write(OBJ_TREE, subdata, sublen, &subtree_id) != 0) {
                    free(subdata);
                    return -1;
                }
                free(subdata);

                TreeEntry *entry = &root.entries[root.count++];
                entry->mode = MODE_DIR;
                entry->hash = subtree_id;
                strncpy(entry->name, dirname, sizeof(entry->name) - 1);
                entry->name[sizeof(entry->name) - 1] = '\0';
            }
        }
    }

    void *root_data = NULL;
    size_t root_len = 0;

    if (tree_serialize(&root, &root_data, &root_len) != 0) return -1;
    if (object_write(OBJ_TREE, root_data, root_len, id_out) != 0) {
        free(root_data);
        return -1;
    }

    free(root_data);
    return 0;
}
