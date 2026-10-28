// phase 0: what memory.cpp needs from the kernel, on Darwin. reserve 4 GB, alias 64 MB at two offsets, check coherence
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "spike.h"

#define GB4 (1ull << 32)
#define RAM (64u << 20)
#define CONTIG 0x80000000ull
#define WC 0xF0000000ull

static char* g_out;
static size_t g_cap, g_len;
static void P(const char* f, ...) {
    va_list a;
    va_start(a, f);
    if (g_len < g_cap) g_len += vsnprintf(g_out + g_len, g_cap - g_len, f, a);
    va_end(a);
}

// one alias strategy: fills src (the RAM backing), then maps it at both windows inside base
typedef int (*alias_fn)(uint8_t* base);

static int check_alias(uint8_t* base, const char* name) {
    volatile uint32_t* a = (volatile uint32_t*)(base + CONTIG);
    volatile uint32_t* b = (volatile uint32_t*)(base + WC);
    a[0] = 0x11223344;
    b[1024] = 0x55667788;
    a[RAM / 4 - 1] = 0xCAFEBABE;
    int ok = b[0] == 0x11223344 && a[1024] == 0x55667788 && b[RAM / 4 - 1] == 0xCAFEBABE;
    P("  %-28s coherent: %s\n", name, ok ? "YES" : "NO");
    return ok;
}

// A: vm_allocate + vm_remap(copy=FALSE) onto the reserved windows
static int alias_remap(uint8_t* base) {
    vm_address_t src = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &src, RAM, VM_FLAGS_ANYWHERE);
    if (kr) { P("  remap: allocate failed %d (%s)\n", kr, mach_error_string(kr)); return 0; }
    int ok = 1;
    for (uint64_t off : (uint64_t[]){CONTIG, WC}) {
        vm_address_t dst = (vm_address_t)(base + off);
        vm_prot_t cur, max;
        kr = vm_remap(mach_task_self(), &dst, RAM, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), src, FALSE,
                           &cur, &max, VM_INHERIT_NONE);
        if (kr) { P("  remap: remap@%llx failed %d (%s)\n", off, kr, mach_error_string(kr)); ok = 0; }
    }
    // the allocation itself is a third view, drop it by keeping it: the two windows share its pages
    return ok && check_alias(base, "vm_remap");
}

// B: memory entry (named port) + vm_map at fixed addresses
static int alias_entry(uint8_t* base) {
    memory_object_size_t sz = RAM;
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &sz, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &port, MACH_PORT_NULL);
    if (kr) { P("  entry: make_memory_entry failed %d (%s)\n", kr, mach_error_string(kr)); return 0; }
    int ok = 1;
    for (uint64_t off : (uint64_t[]){CONTIG, WC}) {
        vm_address_t dst = (vm_address_t)(base + off);
        kr = vm_map(mach_task_self(), &dst, RAM, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, port, 0, FALSE,
                         VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr) { P("  entry: map@%llx failed %d (%s)\n", off, kr, mach_error_string(kr)); ok = 0; }
    }
    return ok && check_alias(base, "memory_entry + vm_map");
}

// C: shm_open
static int alias_shm(uint8_t* base) {
    char name[64];
    snprintf(name, sizeof name, "/blk%d", getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { P("  shm: shm_open failed errno %d (%s)\n", errno, strerror(errno)); return 0; }
    shm_unlink(name);
    if (ftruncate(fd, RAM)) { P("  shm: ftruncate failed errno %d\n", errno); close(fd); return 0; }
    int ok = 1;
    for (uint64_t off : (uint64_t[]){CONTIG, WC})
        if (mmap(base + off, RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
            P("  shm: mmap@%llx failed errno %d (%s)\n", off, errno, strerror(errno));
            ok = 0;
        }
    close(fd);
    return ok && check_alias(base, "shm_open + mmap");
}

// 4 KB commit steps and decommit zeroing, as memory.cpp's Commit/Decommit do them
static void granularity(uint8_t* base) {
    uint8_t* p = base + 0x10000;  // guest user low area
    P("  hw page size: %lu (vm_page_size %llu)\n", (unsigned long)getpagesize(), (unsigned long long)vm_page_size);
    int r = mprotect(p, 0x1000, PROT_READ | PROT_WRITE);
    P("  mprotect 4 KB at 16K-aligned addr: %s (errno %d)\n", r ? "FAIL" : "ok", r ? errno : 0);
    r = mprotect(p + 0x5000, 0x1000, PROT_READ | PROT_WRITE);
    P("  mprotect 4 KB at 4K-aligned, not 16K: %s (errno %d)\n", r ? "FAIL" : "ok", r ? errno : 0);
    memset(p, 0xAB, 0x4000);
    mprotect(p, 0x4000, PROT_READ | PROT_WRITE);
    memset(p, 0xAB, 0x4000);
    madvise(p, 0x4000, MADV_DONTNEED);
    P("  after MADV_DONTNEED page reads 0x%02X (0x00 means zeroed, as memory.cpp assumes)\n", p[0]);
    memset(p, 0xAB, 0x4000);
    void* q = mmap(p, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    mprotect(p, 0x4000, PROT_READ | PROT_WRITE);
    P("  after MAP_FIXED|MAP_ANON re-map page reads 0x%02X (%s)\n", p[0], q == MAP_FAILED ? "mmap failed" : "zeroed");
    mprotect(p, 0x4000, PROT_NONE);
}

// touch every 16 KB page of the aliased RAM and a 256 MB user range to see what the memory limit does
static void touch(uint8_t* base) {
    for (uint64_t o = 0; o < RAM; o += 0x4000) base[CONTIG + o] = 1;
    P("  touched all 64 MB of RAM view: ok\n");
}

const char* spike_run(char* out, size_t cap, int hint_mode) {
    g_out = out; g_cap = cap; g_len = 0;
    out[0] = 0;
    char model[64] = ""; size_t n = sizeof model;
    sysctlbyname("hw.machine", model, &n, NULL, 0);
    uint64_t mem = 0; n = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &n, NULL, 0);
    P("machine %s, %llu MB RAM\n", model, (unsigned long long)(mem >> 20));
#if TARGET_OS_IPHONE
    P("os_proc_available_memory at start: %zu MB\n", os_proc_available_memory() >> 20);
#endif

    // reservation: the way memory.cpp does it, then plain anywhere, then vm_allocate
    static const struct { const char* name; uint64_t size; } sizes[] = {{"4 GB", GB4}, {"2 GB", GB4 / 2}, {"1 GB", GB4 / 4}};
    uint8_t* base = NULL;
    for (int i = 0; i < 3 && !base; i++) {
        void* p = mmap((void*)0x100000000ull, sizes[i].size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        P("reserve %s via mmap(hint 0x100000000, PROT_NONE): %s", sizes[i].name, p == MAP_FAILED ? "FAIL" : "ok");
        if (p == MAP_FAILED) P(" errno %d (%s)", errno, strerror(errno));
        else P(" at %p%s", p, p == (void*)0x100000000ull ? "" : " (hint ignored)");
        P("\n");
        if (p != MAP_FAILED) { if (i == 0) base = (uint8_t*)p; else munmap(p, sizes[i].size); }
        if (i == 0 && !base) {
            vm_address_t a = 0;
            kern_return_t kr = vm_allocate(mach_task_self(), &a, sizes[i].size, VM_FLAGS_ANYWHERE);
            P("reserve %s via vm_allocate (RW, not PROT_NONE): %s", sizes[i].name, kr ? "FAIL" : "ok");
            if (kr) P(" %d (%s)", kr, mach_error_string(kr)); else vm_deallocate(mach_task_self(), a, sizes[i].size);
            P("\n");
        }
    }
    if (!base) { P("RESULT: cannot reserve 4 GB\n"); return out; }
    P("RESULT: 4 GB reserved at %p\n", base);

    P("aliasing 64 MB at +0x80000000 and +0xF0000000:\n");
    int a = alias_remap(base);
    int b = alias_entry(base);  // overwrites the windows again, independent result
    int c = alias_shm(base);
    P("RESULT alias: remap=%d entry=%d shm=%d\n", a, b, c);
    if (c || b || a) touch(base);
    granularity(base);
#if TARGET_OS_IPHONE
    P("os_proc_available_memory at end: %zu MB\n", os_proc_available_memory() >> 20);
#endif
    return out;
}
