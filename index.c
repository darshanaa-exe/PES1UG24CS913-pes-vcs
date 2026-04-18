// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration for object_write used in index_add
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here.
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index.
//
// Opens INDEX_FILE for reading. If it doesn't exist, returns 0 with an empty
// index (count = 0). Each line is parsed as:
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
// hex_to_hash converts the hex string into an ObjectID.
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    FILE *fp;
    char hex[HASH_HEX_SIZE + 1];

    if (!index) return -1;
    index->count = 0;

    fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        // Missing index file is not an error; just start empty
        return 0;
    }

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *entry = &index->entries[index->count];

        int n = fscanf(fp, "%o %64s %lu %u %511[^\n]\n",
                       &entry->mode,
                       hex,
                       &entry->mtime_sec,
                       &entry->size,
                       entry->path);

        if (n == EOF) break;
        if (n != 5) {
            fclose(fp);
            return -1;
        }

        if (hex_to_hash(hex, &entry->hash) != 0) {
            fclose(fp);
            return -1;
        }

        index->count++;
    }

    fclose(fp);
    return 0;
}

// Helper comparator for qsort: sorts IndexEntry structs alphabetically by path.
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically.
//
// Copies the index, sorts entries by path, writes to a temp file
// (.pes/index.tmp), calls fsync, then renames over the real index.
// Each line is: <mode-octal> <hex-hash> <mtime> <size> <path>
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    FILE *fp;
    char tmp_path[512];
    char hex[HASH_HEX_SIZE + 1];

    if (!index) return -1;

    // Heap-allocate sorted copy: Index is ~5.4 MB, and the caller already
    // has an Index on their stack frame — a second one would overflow 8 MB.
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    fp = fopen(tmp_path, "w");
    if (!fp) { free(sorted); return -1; }

    for (int i = 0; i < sorted->count; i++) {
        hash_to_hex(&sorted->entries[i].hash, hex);

        fprintf(fp, "%o %s %lu %u %s\n",
                sorted->entries[i].mode,
                hex,
                sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }

    // Flush userspace buffers, sync to disk, then atomically replace old index
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(sorted);

    if (rename(tmp_path, INDEX_FILE) != 0) return -1;

    return 0;
}

// Stage a file for the next commit.
//
// Reads the file at `path`, writes its contents as an OBJ_BLOB to the object
// store, then adds or updates the index entry with the new blob hash, mtime,
// size, and mode. Calls index_save to persist the updated index.
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    FILE *fp;
    struct stat st;
    void *buffer = NULL;
    size_t size;
    ObjectID blob_id;
    IndexEntry *entry;

    if (!index || !path) return -1;

    // Verify the path exists and is a regular file
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;

    fp = fopen(path, "rb");
    if (!fp) return -1;

    size = (size_t)st.st_size;
    // Allocate at least 1 byte so malloc(0) is never called
    buffer = malloc(size ? size : 1);
    if (!buffer) {
        fclose(fp);
        return -1;
    }

    if (size > 0 && fread(buffer, 1, size, fp) != size) {
        free(buffer);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    // Write the file contents as a blob object and get back its hash
    if (object_write(OBJ_BLOB, buffer, size, &blob_id) != 0) {
        free(buffer);
        return -1;
    }

    free(buffer);

    // Reuse existing entry if path is already staged, otherwise append
    entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    // Populate / update the entry fields
    entry->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash      = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size      = (uint32_t)st.st_size;
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';

    return index_save(index);
}
