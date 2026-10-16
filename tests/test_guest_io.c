// SPDX-License-Identifier: GPL-2.0-or-later
/* File reads and writes into guest memory that GPU page tracking protects
 * (runtime_memory_guest_fill/drain): the kernel's copy takes no fault, so before these a read
 * into a write-protected page came back short (or failed) and the guest saw a successful short
 * read (Uncharted 2 streaming, issue #101). Also: the backing view of a split flexible mapping.
 * Windows: ninja guest-io-test (gpu/CMakeLists.txt); Linux: bash build.sh --test. */
#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <stdlib.h>
#endif

/* The settings menu of the GPU library restarts through probe.c, which tests do not link. */
void runtime_restart(void) { abort(); }
/* GPU page tracking's protection (runtime_memory.c; declared by the GPU library). */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);

typedef int32_t (ABI *Allocate)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
typedef int32_t (ABI *MapDirect)(void **, uint64_t, int, int, int64_t, uint64_t);
typedef int32_t (ABI *MapFlexible)(void **, uint64_t, int, int);
typedef int32_t (ABI *Unmap)(void *, uint64_t);
/* NIDs: names are only known once the game's import table is loaded. */
#define GET(type, nid) ((type)runtime_memory_resolve(nid))
#define PAGE 16384u
#define SIZE (4 * PAGE)

static unsigned char pattern(uint64_t i) { return (unsigned char)(i * 131 + 7); }

int main(int argc, char **argv) {
    (void)argc;
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR); /* a failed assert prints instead of opening a dialog */
#endif
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s", argv[0]);
    char *slash = strrchr(dir, '/'), *back = strrchr(dir, '\\');
    if (back > slash) slash = back;
    if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");
    char user[600];
    snprintf(user, sizeof(user), "%s/guest-io-user", dir);
    runtime_file_configure(dir, user); /* /temp0 is <user>/temp0, writable */
    snprintf(path, sizeof(path), "%s/temp0/guest-io-test.bin", user);
    FILE *f = fopen(path, "wb");
    assert(f);
    for (uint64_t i = 0; i < SIZE; ++i) fputc(pattern(i), f);
    fclose(f);

    Allocate allocate = GET(Allocate, "rTXw65xmLIA#p#J");
    MapDirect map = GET(MapDirect, "L-Q3LEjIbgA#p#J");
    MapFlexible flexible = GET(MapFlexible, "IWIBBdTHit4#p#J");
    Unmap unmap = GET(Unmap, "cQke9UuBQOk#p#J");
    assert(allocate && map && flexible && unmap);
    int64_t phys = -1;
    assert(allocate(0, 1 << 30, SIZE, PAGE, 0, &phys) == 0);
    void *guest = NULL;
    assert(map(&guest, SIZE, 3, 0, phys, PAGE) == 0);
    unsigned char *g = guest;
    const uintptr_t at = (uintptr_t)guest;

    /* Reads: the second half write-protected as the GPU tracker leaves it (read-only). */
    memset(g, 0, SIZE);
    runtime_memory_gpu_protect(at + SIZE / 2, SIZE / 2, 1, 0);
    const int64_t fd = runtime_file_open("/temp0/guest-io-test.bin", 0, 0);
    assert(fd >= 0);
    assert(runtime_file_pread((int)fd, g, SIZE, 0) == SIZE);
    for (uint64_t i = 0; i < SIZE; ++i) assert(g[i] == pattern(i));
    /* Sequential reads across the protected boundary, and at the end of the file. */
    memset(g, 0, SIZE / 2);
    runtime_memory_gpu_protect(at, SIZE, 1, 1);
    memset(g, 0, SIZE);
    runtime_memory_gpu_protect(at + PAGE, 2 * PAGE, 1, 0);
    assert(runtime_file_lseek((int)fd, 0, 0) == 0);
    assert(runtime_file_read((int)fd, g, PAGE + 100) == PAGE + 100);
    assert(runtime_file_read((int)fd, g + PAGE + 100, SIZE) == SIZE - (PAGE + 100));
    assert(runtime_file_read((int)fd, g, PAGE) == 0);
    for (uint64_t i = 0; i < SIZE; ++i) assert(g[i] == pattern(i));
    runtime_memory_gpu_protect(at, SIZE, 1, 1);
    runtime_file_close((int)fd);

    /* Writes from pages with no access at all (GPU data pending: a read fault's state). */
    for (uint64_t i = 0; i < SIZE; ++i) g[i] = (unsigned char)~pattern(i);
    runtime_memory_gpu_protect(at + PAGE, PAGE, 0, 0);
    const int64_t out = runtime_file_open("/temp0/guest-io-test.bin", 2, 0);
    assert(out >= 0);
    assert(runtime_file_write((int)out, g, SIZE) == SIZE);
    runtime_memory_gpu_protect(at, SIZE, 1, 1);
    runtime_file_close((int)out);
    f = fopen(path, "rb");
    assert(f);
    for (uint64_t i = 0; i < SIZE; ++i) assert(fgetc(f) == (unsigned char)~pattern(i));
    fclose(f);

    /* An unmapped guest destination fails instead of reading anywhere. */
    const int64_t again = runtime_file_open("/temp0/guest-io-test.bin", 0, 0);
    assert(again >= 0);
    assert(unmap(guest, SIZE) == 0);
    assert(runtime_file_pread((int)again, guest, PAGE, 0) == -EFAULT);

    /* A flexible mapping split by a partial unmap: its right part keeps its own pages. */
    void *flex = NULL;
    assert(flexible(&flex, 2 * PAGE, 3, 0) == 0);
    unsigned char *x = flex;
    memset(x, 0x11, 2 * PAGE);
    assert(unmap(x, PAGE) == 0);
    assert(runtime_file_pread((int)again, x + PAGE, PAGE, 0) == PAGE);
    for (uint64_t i = 0; i < PAGE; ++i) assert(x[PAGE + i] == (unsigned char)~pattern(i));
    assert(unmap(x + PAGE, PAGE) == 0);
    runtime_file_close((int)again);
    remove(path);
    puts("Guest I/O: PASS (reads into protected pages, writes from no-access pages, unmapped, split flexible)");
    return 0;
}
