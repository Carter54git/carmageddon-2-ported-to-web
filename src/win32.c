/* carma2web: high-level emulation of the Win32 API used by CARMA2_HW.EXE */
#define _GNU_SOURCE
#include "emu.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define API(n) static uint32_t n##_(X86Cpu* c, uint32_t* a)
#define S(i) ((const char*)G2H(a[i]))
#define P32(i) ((uint32_t*)G2H(a[i]))
#define UNUSED (void)c; (void)a

char gEmu_cwd[260];
static uint32_t gLast_error;

/* ------------------------------------------------------------------ handles */
enum { H_FREE, H_FILE, H_STD, H_FIND, H_MAP, H_HEAP, H_MODULE, H_EVENT };
typedef struct {
    int type;
    int fd;
    char path[512];
    /* find */
    char** names;
    int nnames, pos;
    char dir[512];
    /* mapping */
    uint32_t file_handle;
    int written;
} Handle;
void (*gEmu_file_written)(const char* host_path);
#define MAX_HANDLES 256
static Handle gHandles[MAX_HANDLES];
#define HVAL(i) (0x100u + (uint32_t)(i) * 4u)
#define HIDX(h) (((h) - 0x100u) / 4u)

static int new_handle(int type) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (gHandles[i].type == H_FREE) {
            memset(&gHandles[i], 0, sizeof(Handle));
            gHandles[i].type = type;
            gHandles[i].fd = -1;
            return i;
        }
    }
    emu_fatal("out of handles");
    return 0;
}
static Handle* get_handle(uint32_t h) {
    if (h < 0x100 || HIDX(h) >= MAX_HANDLES) return NULL;
    Handle* r = &gHandles[HIDX(h)];
    return r->type == H_FREE ? NULL : r;
}

/* ------------------------------------------------------------------ kernel32: process/env */
static uint32_t gCmdline, gEnv, gEnvW;
API(GetVersion) { UNUSED; return 0xC0000A04u; } /* Windows 98 */
API(GetCommandLineA) { UNUSED; return gCmdline; }
API(GetStartupInfoA) { UNUSED; memset(P32(0), 0, 68); P32(0)[0] = 68; return 0; }
API(GetEnvironmentStrings) { UNUSED; return gEnv; }
API(GetEnvironmentStringsW) { UNUSED; return gEnvW; }
API(FreeEnvironmentStrings) { UNUSED; return 1; }
API(GetModuleFileNameA) {
    UNUSED;
    snprintf((char*)G2H(a[1]), a[2], "C:\\CARMA2\\CARMA2_HW.EXE");
    return (uint32_t)strlen((char*)G2H(a[1]));
}
static uint32_t gModules[16];
static char gModule_names[16][64];
static int gNum_modules;
API(GetModuleHandleA) {
    UNUSED;
    if (a[0] == 0) return 0x400000;
    for (int i = 0; i < gNum_modules; i++) if (strcasecmp(gModule_names[i], S(0)) == 0) return gModules[i];
    return 0;
}
API(LoadLibraryA) {
    UNUSED;
    for (int i = 0; i < gNum_modules; i++) if (strcasecmp(gModule_names[i], S(0)) == 0) return gModules[i];
    if (gNum_modules >= 16) return 0;
    emu_log("LoadLibraryA(%s)", S(0));
    {
        static const char* known[] = { "glide2x", "kernel32", "user32", "gdi32", "advapi32", "winmm", "dsound", "dinput", "ddraw", NULL };
        const char* base = strrchr(S(0), '\\');
        base = base ? base + 1 : S(0);
        int ok = 0;
        for (int i = 0; known[i]; i++) if (strncasecmp(base, known[i], strlen(known[i])) == 0) ok = 1;
        if (!ok) { gLast_error = 126; return 0; }
    }
    snprintf(gModule_names[gNum_modules], 64, "%s", S(0));
    gModules[gNum_modules] = 0x7F000000u + gNum_modules * 0x10000u;
    return gModules[gNum_modules++];
}
API(FreeLibrary) { UNUSED; return 1; }
API(GetProcAddress) {
    UNUSED;
    const char* dll = "?";
    for (int i = 0; i < gNum_modules; i++) if (gModules[i] == a[0]) dll = gModule_names[i];
    if (a[0] == 0x400000) dll = "carma2_hw.exe";
    char nm[32];
    const char* name = nm;
    if (a[1] < 0x10000) snprintf(nm, sizeof(nm), "#%u", a[1]);
    else name = S(1);
    uint32_t t = emu_resolve_import(dll, name);
    emu_log("GetProcAddress(%s, %s) -> %08x", dll, name, t);
    return t;
}
API(GetCurrentProcess) { UNUSED; return 0xFFFFFFFFu; }
API(ExitProcess) { UNUSED; emu_log("ExitProcess(%d)", a[0]); c->halted = 1; return 0; }
API(TerminateProcess) { UNUSED; emu_log("TerminateProcess(%d)", a[1]); c->halted = 1; return 1; }
API(GetLastError) { UNUSED; return gLast_error; }
API(SetLastError) { UNUSED; gLast_error = a[0]; return 0; }
API(SetHandleCount) { UNUSED; return a[0]; }
API(GetStdHandle) { UNUSED; return 0x10 + (uint32_t)(-10 - (int32_t)a[0]) * 4; }
API(SetStdHandle) { UNUSED; return 1; }
API(GetFileType) {
    UNUSED;
    if (a[0] >= 0x10 && a[0] < 0x20) return 2;
    return get_handle(a[0]) ? 1 : 0;
}
API(SetEnvironmentVariableA) { UNUSED; return 1; }
API(SetConsoleCtrlHandler) { UNUSED; return 1; }
API(PeekNamedPipe) { UNUSED; return 0; }
API(IsBadReadPtr) { UNUSED; return a[0] == 0; }
API(GetSystemDefaultLangID) { UNUSED; return 0x409; }
const char* gEmu_computer_name; /* the default player name in network games */
API(GetComputerNameA) {
    UNUSED;
    char nm[16];
    snprintf(nm, sizeof(nm), "%s", gEmu_computer_name && *gEmu_computer_name ? gEmu_computer_name : "CARMA");
    strcpy((char*)G2H(a[0]), nm);
    *P32(1) = (uint32_t)strlen(nm);
    return 1;
}
API(GetSystemInfo) {
    UNUSED;
    uint32_t* s = P32(0);
    memset(s, 0, 36);
    s[1] = 4096; s[2] = 0x10000; s[3] = 0x7FFEFFFF; s[4] = 1; s[5] = 1; s[6] = 586; s[7] = 65536;
    return 0;
}
API(GetSystemDirectoryA) { UNUSED; snprintf((char*)G2H(a[0]), a[1], "C:\\WINDOWS\\SYSTEM"); return 17; }
API(GlobalMemoryStatus) {
    UNUSED;
    uint32_t* s = P32(0);
    s[0] = 32; s[1] = 20; s[2] = 128u << 20; s[3] = 96u << 20; s[4] = 256u << 20; s[5] = 200u << 20; s[6] = 2047u << 20; s[7] = 1800u << 20;
    return 0;
}
API(RaiseException) { UNUSED; emu_fatal("RaiseException(%08x)", a[0]); return 0; }
API(UnhandledExceptionFilter) { UNUSED; emu_fatal("UnhandledExceptionFilter"); return 0; }
API(RtlUnwind) {
    UNUSED;
    /* unlink SEH frames up to the target frame without running handlers */
    if (a[0]) *(uint32_t*)G2H(c->fs_base) = a[0];
    return 0;
}
API(FormatMessageA) { UNUSED; return 0; }
API(Sleep) { UNUSED; emu_yield(); return 0; }

/* ------------------------------------------------------------------ code pages / strings */
API(GetACP) { UNUSED; return 1252; }
API(GetOEMCP) { UNUSED; return 437; }
API(GetCPInfo) {
    UNUSED;
    uint8_t* p = G2H(a[1]);
    memset(p, 0, 20);
    p[0] = 1;
    p[4] = '?';
    return 1;
}
API(MultiByteToWideChar) {
    UNUSED;
    const uint8_t* s = G2H(a[2]);
    int n = (int32_t)a[3];
    if (n < 0) n = (int)strlen((const char*)s) + 1;
    if (a[5] == 0) return n;
    uint16_t* d = G2H(a[4]);
    int m = n < (int)a[5] ? n : (int)a[5];
    for (int i = 0; i < m; i++) d[i] = s[i];
    return m;
}
API(WideCharToMultiByte) {
    UNUSED;
    const uint16_t* s = G2H(a[2]);
    int n = (int32_t)a[3];
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (a[5] == 0) return n;
    uint8_t* d = G2H(a[4]);
    int m = n < (int)a[5] ? n : (int)a[5];
    for (int i = 0; i < m; i++) d[i] = s[i] < 256 ? (uint8_t)s[i] : '?';
    return m;
}
static uint16_t ctype1(unsigned ch) {
    uint16_t t = 0;
    if (ch < 128) {
        if (isupper(ch)) t |= 1;
        if (islower(ch)) t |= 2;
        if (isdigit(ch)) t |= 4;
        if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10;
        if (iscntrl(ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80;
        if (isalpha(ch)) t |= 0x100;
    } else {
        t = 0x10;
    }
    return t;
}
API(GetStringTypeA) {
    UNUSED;
    const uint8_t* s = G2H(a[2]);
    int n = (int32_t)a[3];
    if (n < 0) n = (int)strlen((const char*)s) + 1;
    uint16_t* d = G2H(a[4]);
    for (int i = 0; i < n; i++) d[i] = ctype1(s[i]);
    return 1;
}
API(GetStringTypeW) {
    UNUSED;
    const uint16_t* s = G2H(a[1]);
    int n = (int32_t)a[2];
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    uint16_t* d = G2H(a[3]);
    for (int i = 0; i < n; i++) d[i] = ctype1(s[i]);
    return 1;
}
API(LCMapStringA) {
    UNUSED;
    const uint8_t* s = G2H(a[2]);
    int n = (int32_t)a[3];
    if (n < 0) n = (int)strlen((const char*)s) + 1;
    if (a[5] == 0) return n;
    uint8_t* d = G2H(a[4]);
    for (int i = 0; i < n && i < (int)a[5]; i++) d[i] = (a[1] & 0x100) ? tolower(s[i]) : (a[1] & 0x200) ? toupper(s[i]) : s[i];
    return n;
}
API(LCMapStringW) {
    UNUSED;
    const uint16_t* s = G2H(a[2]);
    int n = (int32_t)a[3];
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (a[5] == 0) return n;
    uint16_t* d = G2H(a[4]);
    for (int i = 0; i < n && i < (int)a[5]; i++) d[i] = s[i] < 128 ? ((a[1] & 0x100) ? tolower(s[i]) : (a[1] & 0x200) ? toupper(s[i]) : s[i]) : s[i];
    return n;
}
API(CompareStringA) {
    UNUSED;
    int n1 = (int32_t)a[3], n2 = (int32_t)a[5];
    const char* s1 = S(2);
    const char* s2 = S(4);
    if (n1 < 0) n1 = (int)strlen(s1);
    if (n2 < 0) n2 = (int)strlen(s2);
    int n = n1 < n2 ? n1 : n2;
    int r = (a[1] & 1) ? strncasecmp(s1, s2, n) : strncmp(s1, s2, n);
    if (r == 0) r = n1 - n2;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}

/* ------------------------------------------------------------------ memory */
static uint32_t gHeap_handle;
static uint32_t heap_handle(void) { if (!gHeap_handle) gHeap_handle = H2G(gcalloc(4096)); return gHeap_handle; }
API(HeapCreate) { UNUSED; return heap_handle(); }
API(HeapDestroy) { UNUSED; return 1; }
API(GetProcessHeap) { UNUSED; return heap_handle(); }
API(HeapAlloc) {
    UNUSED;
    void* p = gmalloc(a[2]);
    if (a[1] & 8) memset(p, 0, a[2]);
    return H2G(p);
}
API(HeapFree) { UNUSED; gfree(G2H(a[2])); return 1; }
API(HeapReAlloc) {
    UNUSED;
    void* p = G2H(a[2]);
    size_t old = gsize(p);
    if (a[1] & 0x10) { /* in place only */
        return a[3] <= old ? a[2] : 0;
    }
    void* q = grealloc(p, a[3]);
    if ((a[1] & 8) && a[3] > old) memset((uint8_t*)q + old, 0, a[3] - old);
    return H2G(q);
}
API(GlobalAlloc) {
    UNUSED;
    void* p = gmalloc(a[1]);
    if (a[0] & 0x40) memset(p, 0, a[1]);
    return H2G(p);
}
API(GlobalFree) { UNUSED; gfree(G2H(a[0])); return 0; }
API(GlobalReAlloc) { UNUSED; return H2G(grealloc(G2H(a[0]), a[1])); }

typedef struct { uint32_t base, size; void* block; } VRegion;
static VRegion gVregions[256];
API(VirtualAlloc) {
    UNUSED;
    uint32_t addr = a[0], size = a[1], type = a[2];
    if (addr != 0) {
        for (int i = 0; i < 256; i++) {
            if (gVregions[i].block && addr >= gVregions[i].base && addr < gVregions[i].base + gVregions[i].size) return addr & ~0xfffu;
        }
        if (!(type & 0x2000)) { gLast_error = 487; return 0; }
    }
    size = (size + 0xfff) & ~0xfffu;
    for (int i = 0; i < 256; i++) {
        if (!gVregions[i].block) {
            uint8_t* b = gmalloc(size + 0x10000);
            uint32_t base = (H2G(b) + 0xffff) & ~0xffffu;
            memset(G2H(base), 0, size);
            gVregions[i].base = base;
            gVregions[i].size = size;
            gVregions[i].block = b;
            return base;
        }
    }
    return 0;
}
API(VirtualFree) {
    UNUSED;
    for (int i = 0; i < 256; i++) {
        VRegion* r = &gVregions[i];
        if (r->block && a[0] >= r->base && a[0] < r->base + r->size) {
            if (a[2] & 0x8000) { gfree(r->block); r->block = NULL; }
            else if (a[2] & 0x4000) memset(G2H(a[0]), 0, a[1]);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ time */
API(QueryPerformanceFrequency) { UNUSED; P32(0)[0] = 1000000; P32(0)[1] = 0; return 1; }
API(QueryPerformanceCounter) {
    UNUSED;
    static unsigned polls;
    if ((++polls & 63) == 0) emu_yield();
    uint64_t t = (uint64_t)(emu_now_ms() * 1000.0);
    P32(0)[0] = (uint32_t)t;
    P32(0)[1] = (uint32_t)(t >> 32);
    return 1;
}
API(timeGetTime) { UNUSED; static unsigned polls; if ((++polls & 63) == 0) emu_yield(); return (uint32_t)emu_now_ms(); }
static void fill_systemtime(uint16_t* st, int local) {
    time_t t = time(NULL);
    struct tm tm;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    st[0] = tm.tm_year + 1900; st[1] = tm.tm_mon + 1; st[2] = tm.tm_wday; st[3] = tm.tm_mday;
    st[4] = tm.tm_hour; st[5] = tm.tm_min; st[6] = tm.tm_sec; st[7] = 0;
}
API(GetLocalTime) { UNUSED; fill_systemtime(G2H(a[0]), 1); return 0; }
API(GetSystemTime) { UNUSED; fill_systemtime(G2H(a[0]), 0); return 0; }
API(GetTimeZoneInformation) { UNUSED; memset(G2H(a[0]), 0, 172); return 0; }
API(FileTimeToLocalFileTime) { UNUSED; memcpy(G2H(a[1]), G2H(a[0]), 8); return 1; }
API(FileTimeToSystemTime) { UNUSED; fill_systemtime(G2H(a[1]), 0); return 1; }
API(GetDateFormatA) { UNUSED; if (a[5]) snprintf((char*)G2H(a[4]), a[5], "01/01/98"); return 9; }
API(GetTimeFormatA) { UNUSED; if (a[5]) snprintf((char*)G2H(a[4]), a[5], "12:00"); return 6; }

/* ------------------------------------------------------------------ files */
static const char* host_path_of(const char* dos, char* buf, size_t n) {
    emu_host_path(dos, buf, n);
    return buf;
}
API(CreateFileA) {
    UNUSED;
    char hp[1024];
    host_path_of(S(0), hp, sizeof(hp));
    uint32_t access = a[1], disp = a[4];
    int flags = 0;
    if ((access & 0x40000000u) && (access & 0x80000000u)) flags = O_RDWR;
    else if (access & 0x40000000u) flags = O_WRONLY;
    else flags = O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;
    case 2: flags |= O_CREAT | O_TRUNC; break;
    case 4: flags |= O_CREAT; break;
    case 5: flags |= O_TRUNC; break;
    }
    int fd = open(hp, flags, 0644);
    if (gEmu_trace || fd < 0) emu_log("CreateFileA(%s) -> %s fd=%d", S(0), hp, fd);
    if (fd < 0) {
        gLast_error = errno == EEXIST ? 80 : errno == ENOENT ? 2 : 5;
        return 0xFFFFFFFFu;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        gLast_error = 5;
        return 0xFFFFFFFFu;
    }
    int h = new_handle(H_FILE);
    gHandles[h].fd = fd;
    snprintf(gHandles[h].path, sizeof(gHandles[h].path), "%s", hp);
    gLast_error = 0;
    return HVAL(h);
}
API(ReadFile) {
    UNUSED;
    Handle* h = get_handle(a[0]);
    if (a[3]) *P32(3) = 0;
    if (!h || h->type != H_FILE) { gLast_error = 6; return 0; }
    ssize_t n = read(h->fd, G2H(a[1]), a[2]);
    if (n < 0) return 0;
    if (a[3]) *P32(3) = (uint32_t)n;
    return 1;
}
API(WriteFile) {
    UNUSED;
    if (a[0] >= 0x10 && a[0] < 0x20) {
        fwrite(G2H(a[1]), 1, a[2], stderr);
        if (a[3]) *P32(3) = a[2];
        return 1;
    }
    Handle* h = get_handle(a[0]);
    if (a[3]) *P32(3) = 0;
    if (!h || h->type != H_FILE) { gLast_error = 6; return 0; }
    ssize_t n = write(h->fd, G2H(a[1]), a[2]);
    if (n < 0) return 0;
    h->written = 1;
    if (a[3]) *P32(3) = (uint32_t)n;
    return 1;
}
API(SetFilePointer) {
    UNUSED;
    Handle* h = get_handle(a[0]);
    if (!h || h->type != H_FILE) { gLast_error = 6; return 0xFFFFFFFFu; }
    off_t off = (int32_t)a[1];
    off_t r = lseek(h->fd, off, a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END);
    if (a[2]) *P32(2) = 0;
    return (uint32_t)r;
}
API(GetFileSize) {
    UNUSED;
    Handle* h = get_handle(a[0]);
    if (!h || h->type != H_FILE) return 0xFFFFFFFFu;
    struct stat st;
    fstat(h->fd, &st);
    if (a[1]) *P32(1) = 0;
    return (uint32_t)st.st_size;
}
API(SetEndOfFile) {
    UNUSED;
    Handle* h = get_handle(a[0]);
    if (!h) return 0;
    return ftruncate(h->fd, lseek(h->fd, 0, SEEK_CUR)) == 0;
}
API(FlushFileBuffers) { UNUSED; return 1; }
API(GetFileInformationByHandle) {
    UNUSED;
    Handle* h = get_handle(a[0]);
    if (!h || h->type != H_FILE) { gLast_error = 6; return 0; }
    struct stat st;
    if (fstat(h->fd, &st) != 0) return 0;
    uint32_t* info = P32(1);
    memset(info, 0, 52);
    info[0] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    /* FILETIME for mtime: 100ns intervals since 1601 */
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    for (int k = 0; k < 3; k++) { info[1 + k * 2] = (uint32_t)ft; info[2 + k * 2] = (uint32_t)(ft >> 32); }
    info[7] = 0x1234;
    info[8] = 0;
    info[9] = (uint32_t)st.st_size;
    info[10] = 1;
    info[12] = (uint32_t)st.st_ino;
    return 1;
}
API(CloseHandle) {
    UNUSED;
    Handle* h = get_handle(a[0]);
    if (!h) return 1;
    if (h->fd >= 0) close(h->fd);
    if (h->type == H_FILE && h->written) { emu_log("file written: %s", h->path); if (gEmu_file_written) gEmu_file_written(h->path); }
    h->type = H_FREE;
    return 1;
}
API(DeleteFileA) { UNUSED; char hp[1024]; return unlink(host_path_of(S(0), hp, sizeof(hp))) == 0; }
API(MoveFileA) {
    UNUSED;
    char h1[1024], h2[1024];
    host_path_of(S(0), h1, sizeof(h1));
    host_path_of(S(1), h2, sizeof(h2));
    return rename(h1, h2) == 0;
}
API(CreateDirectoryA) { UNUSED; char hp[1024]; return mkdir(host_path_of(S(0), hp, sizeof(hp)), 0755) == 0; }
API(GetFileAttributesA) {
    UNUSED;
    char hp[1024];
    struct stat st;
    if (stat(host_path_of(S(0), hp, sizeof(hp)), &st) != 0) { gLast_error = 2; return 0xFFFFFFFFu; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
API(SetFileAttributesA) { UNUSED; return 1; }

static void fill_find_data(uint8_t* fd, const char* dir, const char* name) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    memset(fd, 0, 320);
    if (stat(p, &st) == 0) {
        *(uint32_t*)fd = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        *(uint32_t*)(fd + 32) = (uint32_t)st.st_size;
    }
    snprintf((char*)fd + 44, 260, "%s", name);
    snprintf((char*)fd + 304, 14, "%.13s", name);
}
API(FindFirstFileA) {
    UNUSED;
    char pat[512], dirpart[512], hp[1024];
    snprintf(pat, sizeof(pat), "%s", S(0));
    char* slash = strrchr(pat, '\\');
    if (!slash) slash = strrchr(pat, '/');
    const char* mask;
    if (slash) { *slash = 0; snprintf(dirpart, sizeof(dirpart), "%s", pat); mask = slash + 1; }
    else { dirpart[0] = 0; mask = pat; }
    emu_host_path(dirpart[0] ? dirpart : ".", hp, sizeof(hp));
    DIR* d = opendir(hp);
    if (!d) { gLast_error = 3; return 0xFFFFFFFFu; }
    int h = new_handle(H_FIND);
    Handle* H = &gHandles[h];
    snprintf(H->dir, sizeof(H->dir), "%s", hp);
    char m[260];
    snprintf(m, sizeof(m), "%s", mask);
    if (strcmp(m, "*.*") == 0) strcpy(m, "*");
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (fnmatch(m, e->d_name, FNM_CASEFOLD) == 0) {
            H->names = realloc(H->names, sizeof(char*) * (H->nnames + 1));
            H->names[H->nnames++] = strdup(e->d_name);
        }
    }
    closedir(d);
    if (gEmu_trace) emu_log("FindFirstFileA(%s) -> %d matches", S(0), H->nnames);
    if (H->nnames == 0) { H->type = H_FREE; gLast_error = 2; return 0xFFFFFFFFu; }
    fill_find_data(G2H(a[1]), H->dir, H->names[0]);
    H->pos = 1;
    return HVAL(h);
}
API(FindNextFileA) {
    UNUSED;
    Handle* H = get_handle(a[0]);
    if (!H || H->type != H_FIND || H->pos >= H->nnames) { gLast_error = 18; return 0; }
    fill_find_data(G2H(a[1]), H->dir, H->names[H->pos++]);
    return 1;
}
API(FindClose) {
    UNUSED;
    Handle* H = get_handle(a[0]);
    if (!H) return 0;
    for (int i = 0; i < H->nnames; i++) free(H->names[i]);
    free(H->names);
    H->type = H_FREE;
    return 1;
}
API(GetCurrentDirectoryA) {
    UNUSED;
    char buf[300];
    snprintf(buf, sizeof(buf), "C:\\CARMA2%s%s", gEmu_cwd[0] ? "\\" : "", gEmu_cwd);
    if (a[0] > strlen(buf)) strcpy((char*)G2H(a[1]), buf);
    return (uint32_t)strlen(buf);
}
API(SetCurrentDirectoryA) {
    UNUSED;
    const char* p = S(0);
    if (strncasecmp(p, "C:\\CARMA2", 9) == 0) p += 9;
    while (*p == '\\') p++;
    snprintf(gEmu_cwd, sizeof(gEmu_cwd), "%s", p);
    emu_log("SetCurrentDirectory(%s)", S(0));
    return 1;
}
API(GetFullPathNameA) {
    UNUSED;
    char buf[600];
    const char* p = S(0);
    if (p[1] == ':') snprintf(buf, sizeof(buf), "%s", p);
    else snprintf(buf, sizeof(buf), "C:\\CARMA2%s%s\\%s", gEmu_cwd[0] ? "\\" : "", gEmu_cwd, p);
    if (a[1] > strlen(buf)) strcpy((char*)G2H(a[2]), buf);
    if (a[3]) {
        char* last = strrchr((char*)G2H(a[2]), '\\');
        *P32(3) = last ? H2G(last + 1) : a[2];
    }
    return (uint32_t)strlen(buf);
}
API(GetShortPathNameA) {
    UNUSED;
    size_t n = strlen(S(0));
    if (a[2] > n) memmove(G2H(a[1]), G2H(a[0]), n + 1);
    return (uint32_t)n;
}
API(GetDriveTypeA) { UNUSED; return 3; }
API(GetPrivateProfileStringA) {
    UNUSED;
    const char* def = a[2] ? S(2) : "";
    snprintf((char*)G2H(a[3]), a[4], "%s", def);
    return (uint32_t)strlen((char*)G2H(a[3]));
}
API(GetPrivateProfileSectionA) { UNUSED; if (a[2] >= 2) memset(G2H(a[1]), 0, 2); return 0; }

API(CreateFileMappingA) {
    UNUSED;
    int h = new_handle(H_MAP);
    gHandles[h].file_handle = a[0];
    return HVAL(h);
}
API(MapViewOfFile) {
    UNUSED;
    Handle* m = get_handle(a[0]);
    if (!m || m->type != H_MAP) return 0;
    Handle* f = get_handle(m->file_handle);
    if (!f || f->type != H_FILE) return 0;
    struct stat st;
    fstat(f->fd, &st);
    uint32_t off = a[3];
    uint32_t size = a[4] ? a[4] : (uint32_t)st.st_size - off;
    uint8_t* p = gcalloc(size + 1);
    pread(f->fd, p, size, off);
    return H2G(p);
}
API(UnmapViewOfFile) { UNUSED; gfree(G2H(a[0])); return 1; }

/* ------------------------------------------------------------------ registry */
API(RegOpenKeyExA) {
    UNUSED;
    emu_log("RegOpenKeyExA(%08x, %s)", a[0], a[1] ? S(1) : "");
    if (a[1] && strcasestr(S(1), "CARMAGEDDON2")) { *P32(4) = 0x5C1; return 0; }
    return 2;
}
API(RegCloseKey) { UNUSED; return 0; }
API(RegQueryValueExA) {
    UNUSED;
    const char* name = a[1] ? S(1) : "";
    emu_log("RegQueryValueExA(%s)", name);
    uint8_t buf[64];
    uint32_t type, len;
    if (strcasecmp(name, "ExecutableLocation") == 0 || strcasecmp(name, "SourceLocation") == 0) {
        type = 1; strcpy((char*)buf, ".\\"); len = 3;
    } else if (strcasecmp(name, "Driver") == 0) {
        type = 4; *(uint32_t*)buf = 2; len = 4;
    } else if (strcasecmp(name, "Flags") == 0) {
        type = 4; *(uint32_t*)buf = 0x1f; len = 4;
    } else if (strcasecmp(name, "DrvGuid") == 0) {
        static const uint8_t g[16] = { 0x00, 0xea, 0x2b, 0x47, 0xdf, 0x40, 0xd1, 0x11, 0xa9, 0xdf, 0x00, 0x60, 0x97, 0xc2, 0xed, 0xb2 };
        type = 3; memcpy(buf, g, 16); len = 16;
    } else if (strcasecmp(name, "DevGuid") == 0) {
        static const uint8_t g[16] = { 0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e };
        type = 3; memcpy(buf, g, 16); len = 16;
    } else {
        return 2;
    }
    if (a[3]) *P32(3) = type;
    if (a[5]) {
        uint32_t cap = *P32(5);
        *P32(5) = len;
        if (a[4]) {
            if (cap < len) return 234;
            memcpy(G2H(a[4]), buf, len);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ user32 */
#define HWND_MAIN 0x00010010u
static uint32_t gWndproc;
typedef struct { uint32_t hwnd, msg, wp, lp, time, x, y; } Msg;
#define QSIZE 256
static Msg gQueue[QSIZE];
static int gQhead, gQtail;
static int gMouse_x = 320, gMouse_y = 240, gMouse_buttons;
uint8_t gEmu_keys[256];

static void post(uint32_t msg, uint32_t wp, uint32_t lp) {
    int nt = (gQtail + 1) % QSIZE;
    if (nt == gQhead) return;
    Msg* m = &gQueue[gQtail];
    m->hwnd = HWND_MAIN; m->msg = msg; m->wp = wp; m->lp = lp; m->time = (uint32_t)emu_now_ms();
    m->x = gMouse_x; m->y = gMouse_y;
    gQtail = nt;
}
void emu_post_key(int vk, int sc, int down) {
    if (sc >= 0 && sc < 256) gEmu_keys[sc] = down ? 0x80 : 0;
    if (vk) post(down ? 0x100 : 0x101, vk, 1 | ((uint32_t)(sc & 0xff) << 16) | (down ? 0 : 0xC0000000u));
}
void emu_post_mouse(int x, int y, int buttons) {
    if (x != gMouse_x || y != gMouse_y) post(0x200, buttons, (y << 16) | (x & 0xffff));
    if ((buttons & 1) != (gMouse_buttons & 1)) post((buttons & 1) ? 0x201 : 0x202, buttons, (y << 16) | (x & 0xffff));
    if ((buttons & 2) != (gMouse_buttons & 2)) post((buttons & 2) ? 0x204 : 0x205, buttons, (y << 16) | (x & 0xffff));
    gMouse_x = x; gMouse_y = y; gMouse_buttons = buttons;
}
static uint32_t wndproc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!gWndproc) return 0;
    return emu_call4(gWndproc, hwnd, msg, wp, lp);
}
API(RegisterClassA) {
    UNUSED;
    gWndproc = P32(0)[1];
    emu_log("RegisterClassA wndproc=%08x", gWndproc);
    return 0xC001;
}
API(CreateWindowExA) {
    UNUSED;
    emu_log("CreateWindowExA(%s, %s) %dx%d", S(1), a[2] ? S(2) : "", a[6], a[7]);
    post(0x1C, 1, 0); /* WM_ACTIVATEAPP */
    post(0x06, 1, 0); /* WM_ACTIVATE */
    post(0x07, 0, 0); /* WM_SETFOCUS */
    return HWND_MAIN;
}
API(DestroyWindow) { UNUSED; return 1; }
API(UpdateWindow) { UNUSED; return 1; }
API(ShowCursor) { UNUSED; return a[0] ? 0 : (uint32_t)-1; }
API(SetFocus) { UNUSED; return HWND_MAIN; }
API(SetWindowPos) { UNUSED; return 1; }
API(GetClientRect) { UNUSED; uint32_t* r = P32(1); r[0] = 0; r[1] = 0; r[2] = 640; r[3] = 480; return 1; }
API(ClientToScreen) { UNUSED; return 1; }
API(ScreenToClient) { UNUSED; return 1; }
API(GetSystemMetrics) { UNUSED; return a[0] == 0 ? 640 : a[0] == 1 ? 480 : 0; }
API(GetDC) { UNUSED; return 0xDC01; }
API(ReleaseDC) { UNUSED; return 1; }
API(BeginPaint) { UNUSED; memset(G2H(a[1]), 0, 64); return 0xDC01; }
API(EndPaint) { UNUSED; return 1; }
API(IsIconic) { UNUSED; return 0; }
API(GetForegroundWindow) { UNUSED; return HWND_MAIN; }
API(GetCursorPos) { UNUSED; P32(0)[0] = gMouse_x; P32(0)[1] = gMouse_y; return 1; }
API(SetCapture) { UNUSED; return 0; }
API(ReleaseCapture) { UNUSED; return 1; }
API(LoadIconA) { UNUSED; return 0x1C0; }
API(LoadCursorA) { UNUSED; return 0xC0; }
API(FindWindowA) { UNUSED; return 0; }
API(SetRect) { UNUSED; uint32_t* r = P32(0); r[0] = a[1]; r[1] = a[2]; r[2] = a[3]; r[3] = a[4]; return 1; }
API(MessageBoxA) { UNUSED; emu_log("MessageBox: [%s] %s", a[2] ? S(2) : "", a[1] ? S(1) : ""); return 1; }
API(DefWindowProcA) { UNUSED; return 0; }
API(SendMessageA) { UNUSED; return wndproc(a[0], a[1], a[2], a[3]); }
API(MoveWindow) { UNUSED; return 1; }
API(SetForegroundWindow) { UNUSED; return 1; }

static int gPeek_count;
static int fetch_msg(uint32_t* m, int remove) {
    if (gQhead == gQtail) return 0;
    memcpy(m, &gQueue[gQhead], sizeof(Msg));
    if (remove) gQhead = (gQhead + 1) % QSIZE;
    return 1;
}
API(PeekMessageA) {
    UNUSED;
    int r = fetch_msg(P32(0), a[4] & 1);
    if (!r) {
        /* the game polls the message queue once per frame: good point to return to the browser */
        if (++gPeek_count % 2 == 0) emu_yield();
    }
    return r;
}
API(GetMessageA) {
    UNUSED;
    while (!fetch_msg(P32(0), 1)) {
        emu_yield();
        return 1; /* hand the game an idle message rather than blocking */
    }
    return P32(0)[1] != 0x12;
}
API(TranslateMessage) {
    UNUSED;
    uint32_t* m = P32(0);
    if (m[1] == 0x100) {
        uint32_t vk = m[2];
        int ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = vk + 32;
        else if ((vk >= '0' && vk <= '9') || vk == ' ' || vk == 8 || vk == 13 || vk == 27) ch = vk;
        if (ch) post(0x102, ch, m[3]);
    }
    return 1;
}
API(DispatchMessageA) { UNUSED; uint32_t* m = P32(0); return wndproc(m[0], m[1], m[2], m[3]); }

/* ------------------------------------------------------------------ gdi32 / winmm / misc */
API(GetStockObject) { UNUSED; return 0x600 + a[0]; }
API(GetSystemPaletteEntries) { UNUSED; memset(G2H(a[3]), 0, a[2] * 4); return a[2]; }
API(SetPaletteEntries) { UNUSED; return a[2]; }
API(joyGetPosEx) { UNUSED; return 167; }
/* MCI CD audio: the game's music comes from CD tracks. The host may provide them as files
 * (see gEmu_music_*); without them the drive reports no media. */
int (*gEmu_music_tracks)(void);           /* highest audio track number available, 0 = none */
void (*gEmu_music_play)(int track);
void (*gEmu_music_stop)(void);
void (*gEmu_music_pause)(int paused);
int (*gEmu_music_is_playing)(void);
static int gCd_open, gCd_track, gCd_paused;
int gEmu_mcilog;
API(mciSendCommandA) {
    UNUSED;
    uint32_t msg = a[1], flags = a[2];
    uint32_t* p = a[3] ? P32(3) : NULL;
    int tracks = gEmu_music_tracks ? gEmu_music_tracks() : 0;
    if (getenv("MCILOG") || gEmu_mcilog) emu_log("MCI %04x flags %x item %u from %u", msg, flags, p ? p[2] : 0, p ? p[1] : 0);
    switch (msg) {
    case 0x0803: /* MCI_OPEN */
        if (tracks <= 0) return 0x114; /* MCIERR_DEVICE_NOT_READY */
        if (p) p[1] = 1;               /* wDeviceID */
        gCd_open = 1;
        emu_log("CD audio: %d tracks from music files", tracks - 1);
        return 0;
    case 0x0804: /* MCI_CLOSE */
        if (gEmu_music_stop) gEmu_music_stop();
        gCd_open = 0;
        return 0;
    case 0x080D: /* MCI_SET */
        return 0;
    case 0x0814: { /* MCI_STATUS */
        if (!gCd_open || !p) return 0x101;
        uint32_t item = p[2];
        int playing = gEmu_music_is_playing && gEmu_music_is_playing();
        switch (item) {
        case 3: p[1] = tracks > 12 ? tracks : 12; break;     /* number of tracks (track 1 = data); missing ones are mapped by the host */
        case 5: p[1] = 1; break;                             /* media present */
        case 4: p[1] = playing ? 526 : gCd_paused ? 529 : 525; break; /* mode play/pause/stop */
        case 0x4001: p[1] = (p[3] <= 1) ? 1089 : 1088; break; /* track type: other/audio */
        case 1: p[1] = 0x00030000; break;                    /* length: 3 minutes (MSF) */
        case 2: p[1] = (uint32_t)gCd_track; break;           /* position (TMSF track) */
        case 8: p[1] = 1; break;                             /* ready */
        default: p[1] = 0; break;
        }
        return 0;
    }
    case 0x0806: { /* MCI_PLAY */
        int track = gCd_track ? gCd_track : 2;
        if (p && (flags & 4)) track = p[1] & 0xff; /* MCI_FROM, TMSF */
        if (track < 2) track = 2;
        gCd_track = track;
        gCd_paused = 0;
        if (gEmu_music_play) gEmu_music_play(track);
        return 0;
    }
    case 0x0808: /* MCI_STOP */
        gCd_paused = 0;
        if (gEmu_music_stop) gEmu_music_stop();
        return 0;
    case 0x0809: /* MCI_PAUSE */
        gCd_paused = 1;
        if (gEmu_music_pause) gEmu_music_pause(1);
        return 0;
    case 0x0855: /* MCI_RESUME */
        gCd_paused = 0;
        if (gEmu_music_pause) gEmu_music_pause(0);
        return 0;
    }
    return gCd_open ? 0 : 0x113;
}
API(SmackOpen) { UNUSED; emu_log("SmackOpen(%s) -> no video", a[0] > 0x10000 ? S(0) : "?"); return 0; }
API(SmackRet0) { UNUSED; return 0; }
API(IFRet0) { UNUSED; return 0; }


/* ---- WSOCK32: the game's IPX datagram networking, carried over a relay (web/main.c: WebSocket) ----
 * Addresses are SOCKADDR_IPX (family 6, net[4], node[6], socket[2]); every browser tab is one IPX
 * node, the relay forwards datagrams to a node or to everybody (node ff:ff:ff:ff:ff:ff). */
int (*gEmu_net_open)(uint8_t* node6);                                   /* 0 = no network */
void (*gEmu_net_send)(const uint8_t* node6, const uint8_t* data, int len);
int (*gEmu_net_recv)(uint8_t* node6, uint8_t* data, int max);           /* -1 = nothing pending */
int (*gEmu_net_pending)(void);
int gEmu_netlog;
static uint8_t gNet_node[6], gNet_sock[2];
static int gNet_up, gWsa_err;
static uint32_t gNet_ntoa;
#define WSAEWOULDBLOCK 10035
#define NETLOG(...) do { if (gEmu_netlog) emu_log(__VA_ARGS__); } while (0)
static void ipx_addr_out(uint32_t ga, const uint8_t* node) {
    uint8_t* p = G2H(ga);
    p[0] = 6; p[1] = 0;
    memset(p + 2, 0, 4);
    memcpy(p + 6, node, 6);
    memcpy(p + 12, gNet_sock, 2);
}
API(WSAStartup) {
    UNUSED;
    if (!gNet_up) {
        if (!gEmu_net_open || !gEmu_net_open(gNet_node)) { emu_log("WSAStartup: no network"); return 10091; }
        gNet_up = 1;
        emu_log("network: IPX node %02x%02x%02x%02x%02x%02x", gNet_node[0], gNet_node[1], gNet_node[2], gNet_node[3], gNet_node[4], gNet_node[5]);
    }
    if (a[1]) {
        uint8_t* d = G2H(a[1]);
        memset(d, 0, 400);
        d[0] = 1; d[1] = 1; d[2] = 1; d[3] = 1;
        strcpy((char*)d + 4, "WinSock 1.1 (relay)");
        strcpy((char*)d + 4 + 257, "Running");
        d[390] = 64;
        d[392] = 0; d[393] = 4; /* max UDP datagram 1024 */
    }
    return 0;
}
API(WSACleanup) { UNUSED; return 0; }
API(WSAGetLastError) { UNUSED; return (uint32_t)gWsa_err; }
API(wsock_socket) {
    UNUSED;
    NETLOG("socket(%d, %d, %d)", a[0], a[1], a[2]);
    if (!gNet_up || a[0] != 6) { gWsa_err = 10047; return 0xffffffffu; }
    return 0x51;
}
API(wsock_setsockopt) { UNUSED; return 0; }
API(wsock_ioctlsocket) {
    UNUSED;
    if (a[1] == 0x4004667fu && a[2]) *P32(2) = gEmu_net_pending ? (uint32_t)gEmu_net_pending() : 0; /* FIONREAD */
    return 0;
}
API(wsock_bind) {
    UNUSED;
    if (a[1]) memcpy(gNet_sock, (uint8_t*)G2H(a[1]) + 12, 2);
    NETLOG("bind socket %02x%02x", gNet_sock[0], gNet_sock[1]);
    return 0;
}
API(wsock_getsockname) {
    UNUSED;
    if (a[1]) ipx_addr_out(a[1], gNet_node);
    if (a[2]) *P32(2) = 14;
    return 0;
}
API(wsock_sendto) {
    UNUSED;
    const uint8_t* to = a[4] ? (const uint8_t*)G2H(a[4]) : NULL;
    int len = (int)a[2];
    if (!gNet_up || !to || len < 0) { gWsa_err = 10057; return 0xffffffffu; }
    NETLOG("sendto %02x%02x%02x%02x%02x%02x len=%d", to[6], to[7], to[8], to[9], to[10], to[11], len);
    if (gEmu_net_send) gEmu_net_send(to + 6, G2H(a[1]), len);
    return (uint32_t)len;
}
API(wsock_recvfrom) {
    UNUSED;
    static uint8_t buf[4096];
    uint8_t node[6];
    int n = gNet_up && gEmu_net_recv ? gEmu_net_recv(node, buf, sizeof(buf)) : -1;
    if (n < 0) { gWsa_err = WSAEWOULDBLOCK; return 0xffffffffu; }
    int max = (int)a[2];
    if (n > max) n = max;
    memcpy(G2H(a[1]), buf, (size_t)n);
    if (a[4]) ipx_addr_out(a[4], node);
    if (a[5]) *P32(5) = 14;
    NETLOG("recvfrom %02x%02x%02x%02x%02x%02x len=%d", node[0], node[1], node[2], node[3], node[4], node[5], n);
    return (uint32_t)n;
}
API(wsock_closesocket) { UNUSED; return 0; }
API(wsock_inet_ntoa) {
    UNUSED;
    if (!gNet_ntoa) gNet_ntoa = H2G(gstrdup("0.0.0.0"));
    return gNet_ntoa;
}
API(wsock_htons) { UNUSED; return ((a[0] & 0xff) << 8) | ((a[0] >> 8) & 0xff); }

static const EmuApiDef kWin32[] = {
    { "KERNEL32", "GetVersion", GetVersion_, 0 },
    { "KERNEL32", "GetCommandLineA", GetCommandLineA_, 0 },
    { "KERNEL32", "GetStartupInfoA", GetStartupInfoA_, 1 },
    { "KERNEL32", "GetEnvironmentStrings", GetEnvironmentStrings_, 0 },
    { "KERNEL32", "GetEnvironmentStringsW", GetEnvironmentStringsW_, 0 },
    { "KERNEL32", "FreeEnvironmentStringsA", FreeEnvironmentStrings_, 1 },
    { "KERNEL32", "FreeEnvironmentStringsW", FreeEnvironmentStrings_, 1 },
    { "KERNEL32", "GetModuleFileNameA", GetModuleFileNameA_, 3 },
    { "KERNEL32", "GetModuleHandleA", GetModuleHandleA_, 1 },
    { "KERNEL32", "LoadLibraryA", LoadLibraryA_, 1 },
    { "KERNEL32", "FreeLibrary", FreeLibrary_, 1 },
    { "KERNEL32", "GetProcAddress", GetProcAddress_, 2 },
    { "KERNEL32", "GetCurrentProcess", GetCurrentProcess_, 0 },
    { "KERNEL32", "ExitProcess", ExitProcess_, 1 },
    { "KERNEL32", "TerminateProcess", TerminateProcess_, 2 },
    { "KERNEL32", "GetLastError", GetLastError_, 0 },
    { "KERNEL32", "SetLastError", SetLastError_, 1 },
    { "KERNEL32", "SetHandleCount", SetHandleCount_, 1 },
    { "KERNEL32", "GetStdHandle", GetStdHandle_, 1 },
    { "KERNEL32", "SetStdHandle", SetStdHandle_, 2 },
    { "KERNEL32", "GetFileType", GetFileType_, 1 },
    { "KERNEL32", "SetEnvironmentVariableA", SetEnvironmentVariableA_, 2 },
    { "KERNEL32", "SetConsoleCtrlHandler", SetConsoleCtrlHandler_, 2 },
    { "KERNEL32", "PeekNamedPipe", PeekNamedPipe_, 6 },
    { "KERNEL32", "IsBadReadPtr", IsBadReadPtr_, 2 },
    { "KERNEL32", "GetSystemDefaultLangID", GetSystemDefaultLangID_, 0 },
    { "KERNEL32", "GetComputerNameA", GetComputerNameA_, 2 },
    { "KERNEL32", "GetSystemInfo", GetSystemInfo_, 1 },
    { "KERNEL32", "GetSystemDirectoryA", GetSystemDirectoryA_, 2 },
    { "KERNEL32", "GlobalMemoryStatus", GlobalMemoryStatus_, 1 },
    { "KERNEL32", "RaiseException", RaiseException_, 4 },
    { "KERNEL32", "UnhandledExceptionFilter", UnhandledExceptionFilter_, 1 },
    { "KERNEL32", "RtlUnwind", RtlUnwind_, 4 },
    { "KERNEL32", "FormatMessageA", FormatMessageA_, 7 },
    { "KERNEL32", "Sleep", Sleep_, 1 },
    { "KERNEL32", "GetACP", GetACP_, 0 },
    { "KERNEL32", "GetOEMCP", GetOEMCP_, 0 },
    { "KERNEL32", "GetCPInfo", GetCPInfo_, 2 },
    { "KERNEL32", "MultiByteToWideChar", MultiByteToWideChar_, 6 },
    { "KERNEL32", "WideCharToMultiByte", WideCharToMultiByte_, 8 },
    { "KERNEL32", "GetStringTypeA", GetStringTypeA_, 5 },
    { "KERNEL32", "GetStringTypeW", GetStringTypeW_, 4 },
    { "KERNEL32", "LCMapStringA", LCMapStringA_, 6 },
    { "KERNEL32", "LCMapStringW", LCMapStringW_, 6 },
    { "KERNEL32", "CompareStringA", CompareStringA_, 6 },
    { "KERNEL32", "HeapCreate", HeapCreate_, 3 },
    { "KERNEL32", "HeapDestroy", HeapDestroy_, 1 },
    { "KERNEL32", "GetProcessHeap", GetProcessHeap_, 0 },
    { "KERNEL32", "HeapAlloc", HeapAlloc_, 3 },
    { "KERNEL32", "HeapFree", HeapFree_, 3 },
    { "KERNEL32", "HeapReAlloc", HeapReAlloc_, 4 },
    { "KERNEL32", "GlobalAlloc", GlobalAlloc_, 2 },
    { "KERNEL32", "GlobalFree", GlobalFree_, 1 },
    { "KERNEL32", "GlobalReAlloc", GlobalReAlloc_, 3 },
    { "KERNEL32", "VirtualAlloc", VirtualAlloc_, 4 },
    { "KERNEL32", "VirtualFree", VirtualFree_, 3 },
    { "KERNEL32", "QueryPerformanceFrequency", QueryPerformanceFrequency_, 1 },
    { "KERNEL32", "QueryPerformanceCounter", QueryPerformanceCounter_, 1 },
    { "KERNEL32", "GetLocalTime", GetLocalTime_, 1 },
    { "KERNEL32", "GetSystemTime", GetSystemTime_, 1 },
    { "KERNEL32", "GetTimeZoneInformation", GetTimeZoneInformation_, 1 },
    { "KERNEL32", "FileTimeToLocalFileTime", FileTimeToLocalFileTime_, 2 },
    { "KERNEL32", "FileTimeToSystemTime", FileTimeToSystemTime_, 2 },
    { "KERNEL32", "GetDateFormatA", GetDateFormatA_, 6 },
    { "KERNEL32", "GetTimeFormatA", GetTimeFormatA_, 6 },
    { "KERNEL32", "CreateFileA", CreateFileA_, 7 },
    { "KERNEL32", "ReadFile", ReadFile_, 5 },
    { "KERNEL32", "WriteFile", WriteFile_, 5 },
    { "KERNEL32", "SetFilePointer", SetFilePointer_, 4 },
    { "KERNEL32", "GetFileSize", GetFileSize_, 2 },
    { "KERNEL32", "SetEndOfFile", SetEndOfFile_, 1 },
    { "KERNEL32", "FlushFileBuffers", FlushFileBuffers_, 1 },
    { "KERNEL32", "GetFileInformationByHandle", GetFileInformationByHandle_, 2 },
    { "KERNEL32", "CloseHandle", CloseHandle_, 1 },
    { "KERNEL32", "DeleteFileA", DeleteFileA_, 1 },
    { "KERNEL32", "MoveFileA", MoveFileA_, 2 },
    { "KERNEL32", "CreateDirectoryA", CreateDirectoryA_, 2 },
    { "KERNEL32", "GetFileAttributesA", GetFileAttributesA_, 1 },
    { "KERNEL32", "SetFileAttributesA", SetFileAttributesA_, 2 },
    { "KERNEL32", "FindFirstFileA", FindFirstFileA_, 2 },
    { "KERNEL32", "FindNextFileA", FindNextFileA_, 2 },
    { "KERNEL32", "FindClose", FindClose_, 1 },
    { "KERNEL32", "GetCurrentDirectoryA", GetCurrentDirectoryA_, 2 },
    { "KERNEL32", "SetCurrentDirectoryA", SetCurrentDirectoryA_, 1 },
    { "KERNEL32", "GetFullPathNameA", GetFullPathNameA_, 4 },
    { "KERNEL32", "GetShortPathNameA", GetShortPathNameA_, 3 },
    { "KERNEL32", "GetDriveTypeA", GetDriveTypeA_, 1 },
    { "KERNEL32", "GetPrivateProfileStringA", GetPrivateProfileStringA_, 6 },
    { "KERNEL32", "GetPrivateProfileSectionA", GetPrivateProfileSectionA_, 4 },
    { "KERNEL32", "CreateFileMappingA", CreateFileMappingA_, 6 },
    { "KERNEL32", "MapViewOfFile", MapViewOfFile_, 5 },
    { "KERNEL32", "UnmapViewOfFile", UnmapViewOfFile_, 1 },
    { "ADVAPI32", "RegOpenKeyExA", RegOpenKeyExA_, 5 },
    { "ADVAPI32", "RegCloseKey", RegCloseKey_, 1 },
    { "ADVAPI32", "RegQueryValueExA", RegQueryValueExA_, 6 },
    { "USER32", "RegisterClassA", RegisterClassA_, 1 },
    { "USER32", "CreateWindowExA", CreateWindowExA_, 12 },
    { "USER32", "DestroyWindow", DestroyWindow_, 1 },
    { "USER32", "UpdateWindow", UpdateWindow_, 1 },
    { "USER32", "ShowCursor", ShowCursor_, 1 },
    { "USER32", "SetFocus", SetFocus_, 1 },
    { "USER32", "SetWindowPos", SetWindowPos_, 7 },
    { "USER32", "GetClientRect", GetClientRect_, 2 },
    { "USER32", "ClientToScreen", ClientToScreen_, 2 },
    { "USER32", "ScreenToClient", ScreenToClient_, 2 },
    { "USER32", "GetSystemMetrics", GetSystemMetrics_, 1 },
    { "USER32", "GetDC", GetDC_, 1 },
    { "USER32", "ReleaseDC", ReleaseDC_, 2 },
    { "USER32", "BeginPaint", BeginPaint_, 2 },
    { "USER32", "EndPaint", EndPaint_, 2 },
    { "USER32", "IsIconic", IsIconic_, 1 },
    { "USER32", "GetForegroundWindow", GetForegroundWindow_, 0 },
    { "USER32", "GetCursorPos", GetCursorPos_, 1 },
    { "USER32", "SetCapture", SetCapture_, 1 },
    { "USER32", "ReleaseCapture", ReleaseCapture_, 0 },
    { "USER32", "LoadIconA", LoadIconA_, 2 },
    { "USER32", "LoadCursorA", LoadCursorA_, 2 },
    { "USER32", "FindWindowA", FindWindowA_, 2 },
    { "USER32", "SetRect", SetRect_, 5 },
    { "USER32", "MessageBoxA", MessageBoxA_, 4 },
    { "USER32", "DefWindowProcA", DefWindowProcA_, 4 },
    { "USER32", "SendMessageA", SendMessageA_, 4 },
    { "USER32", "PeekMessageA", PeekMessageA_, 5 },
    { "USER32", "GetMessageA", GetMessageA_, 4 },
    { "USER32", "TranslateMessage", TranslateMessage_, 1 },
    { "USER32", "DispatchMessageA", DispatchMessageA_, 1 },
    { "USER32", "MoveWindow", MoveWindow_, 6 },
    { "USER32", "SetForegroundWindow", SetForegroundWindow_, 1 },
    { "GDI32", "GetStockObject", GetStockObject_, 1 },
    { "GDI32", "GetSystemPaletteEntries", GetSystemPaletteEntries_, 4 },
    { "GDI32", "SetPaletteEntries", SetPaletteEntries_, 4 },
    { "WINMM", "joyGetPosEx", joyGetPosEx_, 2 },
    { "WINMM", "mciSendCommandA", mciSendCommandA_, 4 },
    { "WINMM", "timeGetTime", timeGetTime_, 0 },
    { "smackw32", "_SmackOpen@12", SmackOpen_, 3 },
    { "smackw32", "_SmackSoundUseDirectSound@4", SmackRet0_, 1 },
    { "smackw32", "_SmackUseMMX@4", SmackRet0_, 1 },
    { "smackw32", "_SmackClose@4", SmackRet0_, 1 },
    { "smackw32", "_SmackBlitClose@4", SmackRet0_, 1 },
    { "WSOCK32", "#115", WSAStartup_, 2 },
    { "WSOCK32", "#116", WSACleanup_, 0 },
    { "WSOCK32", "#111", WSAGetLastError_, 0 },
    { "WSOCK32", "#23", wsock_socket_, 3 },
    { "WSOCK32", "#21", wsock_setsockopt_, 5 },
    { "WSOCK32", "#12", wsock_ioctlsocket_, 3 },
    { "WSOCK32", "#2", wsock_bind_, 3 },
    { "WSOCK32", "#6", wsock_getsockname_, 3 },
    { "WSOCK32", "#20", wsock_sendto_, 6 },
    { "WSOCK32", "#17", wsock_recvfrom_, 6 },
    { "WSOCK32", "#3", wsock_closesocket_, 1 },
    { "WSOCK32", "#11", wsock_inet_ntoa_, 1 },
    { "WSOCK32", "#9", wsock_htons_, 1 },
    { "IFORCE2", "_IFLoadProjectFile@8", IFRet0_, 2 },
    { "IFORCE2", "_IFReleaseProject@4", IFRet0_, 1 },
    { "IFORCE2", "_IFCreateEffectStructs@12", IFRet0_, 3 },
    { NULL, NULL, NULL, 0 },
};

void emu_win32_init(void) {
    emu_register_apis(kWin32);
    const char* cmd = getenv("CARMA2_CMDLINE");
    char buf[256];
    snprintf(buf, sizeof(buf), "C:\\CARMA2\\CARMA2_HW.EXE%s%s", cmd ? " " : "", cmd ? cmd : "");
    gCmdline = H2G(gstrdup(buf));
    gEnv = H2G(gcalloc(16));
    gEnvW = H2G(gcalloc(16));
}
