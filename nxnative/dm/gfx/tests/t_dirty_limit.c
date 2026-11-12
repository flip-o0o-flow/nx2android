// The kernel's mapping limit in dirty-page tracking: a write fault whose mprotect (making the page writable) fails must not crash. Batman: Arkham Knight on the RM
// reached vm.max_map_count after 489 s of texture streaming; the page's mprotect failed and the game's write crashed. The rescue makes the whole range writable in one
// call (no split) and records its protected pages as written. Staged: t_mprotect fails a one-page writable call while armed, as the kernel does at the limit (ENOMEM)
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <errno.h>
#include <stdio.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail; static _Atomic int g_armed;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); printf("%-62s expected %-4lld → got %-4lld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); if (w_ != g_) g_fail++; } while (0)
int t_mprotect(void *a, size_t n, int prot) {
    if (atomic_load(&g_armed) && prot == (PROT_READ | PROT_WRITE) && n == (size_t)getpagesize()) { errno = ENOMEM; return -1; }   // a split would pass the limit
    return mprotect(a, n, prot);
}
int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(m, 16 * pg);
    nvn_dirty_protect(m + 4 * pg, 4 * pg);   // a texture's storage, read-only
    uint64_t stamp = nvn_dirty_now();
    atomic_store(&g_armed, 1);
    ((volatile uint8_t *)m)[5 * pg + 3] = 0x77;   // the game writes into it: the fault's one-page mprotect fails
    atomic_store(&g_armed, 0);
    EXPECT("the write went through and reads back", 0x77, m[5 * pg + 3]);
    EXPECT("one rescue counted", 1, (long long)nvn_dirty_rescues());
    EXPECT("the texture counts as written", 1, nvn_dirty_since(m + 4 * pg, 4 * pg, stamp));
    ((volatile uint8_t *)m)[7 * pg] = 0x11;   // the rest of the range is writable too (no further fault)
    EXPECT("another page of the texture is writable", 0x11, m[7 * pg]);
    printf("t_dirty_limit: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
