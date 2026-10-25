/*
 * hotswap_re - safely hot-swap disks, optical drives and other removable devices.
 *
 * A from-scratch reimplementation of the HotSwap! tray applet with stability fixes.
 * Pure Win32 C with no C runtime: it only imports system DLLs that exist on every
 * Windows release from XP to 11 (kernel32, user32, shell32, advapi32, setupapi,
 * ole32, oleaut32). Vista+ only features (offline, Task Scheduler autostart,
 * UIPI message filter, WOW64 check) are used only when the running OS has them.
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600   /* for declarations only; XP-safe at runtime, see above */
#define WINVER       0x0600
#define _WIN32_IE    0x0600
#define COBJMACROS

#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <ntddscsi.h>
#include <ole2.h>
#include <taskschd.h>
#include <dbt.h>
#include <stddef.h>

#include "lang_gen.h"
#include "version.h"

#define IDI_APP 100

/* ------------------------------------------------------------------------- */
/* Strings                                                                    */
/* ------------------------------------------------------------------------- */

/* Strings added by hotswap_re (English only; other languages fall back to these). */
enum {
    S_X_NOTIFY = S_GEN_COUNT, S_X_WOW64, S_X_GONE, S_X_SYSTEM, S_X_NOTFOUND,
    S_X_BUSY, S_X_VETO, S_X_ABOUT_TEXT, S_X_USAGE_EXTRA, S_X_SELECT_NONHS_DOTS,
    S_X_SYSTEM_TAG, S_X_WORKING, S_X_SCAN_DONE, S_X_CR_ERROR, S_X_OFFLINE_NA,
    S_X_LIST_TITLE, S_COUNT
};

static const WCHAR *const g_extStrings[S_COUNT - S_GEN_COUNT] = {
    L"Show notifications",
    L"This is the 32-bit build running on 64-bit Windows.\n\n"
    L"Windows does not allow 32-bit programs to remove devices on 64-bit systems. "
    L"Please use the 64-bit build (x64\\hotswap_re.exe).",
    L"The device '%s' is no longer present. The device list changed after the menu was opened; please try again.",
    L"The device '%s' holds the running Windows installation and cannot be hotswapped.",
    L"No matching device was found for '%s'.",
    L"Another hotswap operation is still in progress.",
    L"Windows refused to stop the device '%s'.",
    L"hotswap_re version " HSR_VER_WSTR L"\n\n"
    L"A from-scratch reimplementation of the HotSwap! applet (originally by Kazuyuki Nakayama), "
    L"with stability fixes for device-list changes.\n\n"
    L"Runs on Windows XP, Vista, 7, 8, 8.1, 10 and 11.",
    L"   -L : List detected devices (DISK numbers, PDO names).\n"
    L"   -? : Show this help.\n\n"
    L"   Exit codes: 0 = success, 1 = device not found,\n"
    L"   2 = removal failed, 3 = invalid arguments or privileges.\n\n"
    L"   After a removal the device stays stopped even when Windows\n"
    L"   rescans, until it is unplugged or 'Scan for hardware changes'\n"
    L"   (or hotswap_re -S) is used.",
    L"Select non-hotswap devices...",
    L" (system)",
    L"working...",
    L"Hardware scan finished.",
    L"Configuration Manager error 0x%02X.",
    L"Offline (Vista or later)",
    L"Detected devices",
};

static int g_lang; /* 0..NLANG-1 */

static const WCHAR *T(int id)
{
    if (id >= S_GEN_COUNT) return g_extStrings[id - S_GEN_COUNT];
    if (g_genStrings[id][g_lang]) return g_genStrings[id][g_lang];
    return g_genStrings[id][0];
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static HINSTANCE g_hInst;
static DWORD     g_osMajor;
static HWND      g_hwnd;
static BOOL      g_quiet;

static void *Alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  Free(void *p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* Bounded append. cap = size of dst in WCHARs. */
static void StrCat(WCHAR *dst, int cap, const WCHAR *src)
{
    int n = lstrlenW(dst);
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
}

static void StrCpy(WCHAR *dst, int cap, const WCHAR *src)
{
    dst[0] = 0;
    StrCat(dst, cap, src);
}

static BOOL StartsWithI(const WCHAR *s, const WCHAR *prefix)
{
    for (; *prefix; s++, prefix++) {
        WCHAR a = *s, b = *prefix;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return FALSE;
    }
    return TRUE;
}

static int ParseInt(const WCHAR *s)
{
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    return (any && !*s) ? v : -1;
}

#ifdef HSR_TEST
/* Test builds print message boxes to stdout (UTF-8) instead of showing them. */
static void Out(const WCHAR *s)
{
    char buf[8192];
    DWORD n = (DWORD)WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof buf, NULL, NULL), w;
    if (n) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n - 1, &w, NULL);
}

static int Msg(const WCHAR *text, const WCHAR *title, UINT flags)
{
    (void)flags;
    Out(L"[");
    Out(title);
    Out(L"]\n");
    Out(text);
    Out(L"\n");
    return IDNO;
}
#else
static int Msg(const WCHAR *text, const WCHAR *title, UINT flags)
{
    if (g_lang == 8) flags |= MB_RTLREADING | MB_RIGHT; /* Hebrew */
    return MessageBoxW(NULL, text, title, flags | MB_SETFOREGROUND | MB_TOPMOST);
}
#endif

/* Formats one '%s' format string (from the language table) with an argument. */
static void Fmt1(WCHAR *out, int cap, const WCHAR *fmt, const WCHAR *arg)
{
    const WCHAR *p;
    out[0] = 0;
    for (p = fmt; *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            StrCat(out, cap, arg);
            p++;
        } else {
            WCHAR c[2] = { *p, 0 };
            StrCat(out, cap, c);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Multi-string helpers                                                       */
/* ------------------------------------------------------------------------- */

static int MszLen(const WCHAR *m) /* in WCHARs, excluding the final terminator */
{
    const WCHAR *p = m;
    while (*p) p += lstrlenW(p) + 1;
    return (int)(p - m);
}

static BOOL MszHas(const WCHAR *m, const WCHAR *s)
{
    for (; *m; m += lstrlenW(m) + 1)
        if (!lstrcmpiW(m, s)) return TRUE;
    return FALSE;
}

static BOOL MszAdd(WCHAR *m, int cap, const WCHAR *s)
{
    int len, need;
    if (MszHas(m, s)) return TRUE;
    len = MszLen(m);
    need = lstrlenW(s) + 1;
    if (len + need + 1 > cap) return FALSE;
    lstrcpyW(m + len, s);
    m[len + need] = 0;
    return TRUE;
}

static void MszDel(WCHAR *m, const WCHAR *s)
{
    WCHAR *r = m, *w = m;
    while (*r) {
        int l = lstrlenW(r) + 1;
        if (lstrcmpiW(r, s)) {
            if (w != r) memmove(w, r, l * sizeof(WCHAR));
            w += l;
        }
        r += l;
    }
    *w = 0;
}

/* ------------------------------------------------------------------------- */
/* Settings (HKCU\Software\hotswap_re)                                        */
/* ------------------------------------------------------------------------- */

#define REG_KEY      L"Software\\hotswap_re"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_NAME     L"hotswap_re Applet"
#define MSZ_CAP      8192

enum { K_FIXED, K_REMOVABLE, K_CDROM, K_OTHER, K_COUNT };

static struct {
    DWORD displayPdo, spinDown, offline, noBalloon, dflags;
} g_set;

static WCHAR g_nonHs[MSZ_CAP];   /* device instance IDs the user marked as hotswap */

static DWORD RegGetDword(const WCHAR *name, DWORD def)
{
    HKEY k;
    DWORD v = def, t, n = sizeof v;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)&v, &n) != ERROR_SUCCESS || t != REG_DWORD)
            v = def;
        RegCloseKey(k);
    }
    return v;
}

static void RegSetDword(const WCHAR *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
        RegCloseKey(k);
    }
}

static void RegGetMsz(const WCHAR *name, WCHAR *buf, int cap)
{
    HKEY k;
    DWORD t, n = (DWORD)(cap - 2) * sizeof(WCHAR);
    buf[0] = buf[1] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)buf, &n) == ERROR_SUCCESS && t == REG_MULTI_SZ) {
            n /= sizeof(WCHAR);
            buf[n] = buf[n + 1] = 0;   /* registry data may lack terminators */
        } else {
            buf[0] = buf[1] = 0;
        }
        RegCloseKey(k);
    }
}

static void RegSetMsz(const WCHAR *name, const WCHAR *m)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE *)m, (MszLen(m) + 1) * sizeof(WCHAR));
        RegCloseKey(k);
    }
}

static int DefaultLanguage(void)
{
    LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_CHINESE:
        return (id == 0x0804 || id == 0x1004) ? 1 : 2;
    case LANG_CZECH:      return 3;
    case LANG_DUTCH:      return 4;
    case LANG_FRENCH:     return 5;
    case LANG_GERMAN:     return 6;
    case LANG_GREEK:      return 7;
    case LANG_HEBREW:     return 8;
    case LANG_HUNGARIAN:  return 9;
    case LANG_ITALIAN:    return 10;
    case LANG_JAPANESE:   return 11;
    case LANG_KOREAN:     return 12;
    case LANG_POLISH:     return 13;
    case LANG_PORTUGUESE: return 14;
    case LANG_RUSSIAN:    return 15;
    case LANG_SPANISH:    return 16;
    case LANG_SWEDISH:    return 17;
    }
    return 0;
}

static void LoadSettings(void)
{
    DWORD lang;
    g_set.displayPdo = RegGetDword(L"DisplayPDO", 0);
    g_set.spinDown   = RegGetDword(L"SpinDown", 1);
    g_set.offline    = RegGetDword(L"Offline", 0);
    g_set.noBalloon  = RegGetDword(L"NoBalloon", 0);
    g_set.dflags     = RegGetDword(L"DFlags", (1 << K_FIXED) | (1 << K_REMOVABLE) | (1 << K_CDROM));
    lang = RegGetDword(L"Language", 0xFFFFFFFF);
    g_lang = (lang < NLANG) ? (int)lang : DefaultLanguage();
    RegGetMsz(L"NonHotSwap", g_nonHs, MSZ_CAP);
}

/* ------------------------------------------------------------------------- */
/* Privileges / platform                                                      */
/* ------------------------------------------------------------------------- */

static BOOL __attribute__((unused)) IsAdmin(void)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID sid;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &sid)) {
        if (!CheckTokenMembership(NULL, sid, &member)) member = FALSE;
        FreeSid(sid);
    }
    return member;
}

static BOOL IsWow64(void)
{
#ifdef _WIN64
    return FALSE;
#else
    typedef BOOL (WINAPI *PFN)(HANDLE, PBOOL);
    PFN f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    BOOL w = FALSE;
    if (f && !f(GetCurrentProcess(), &w)) w = FALSE;
    return w;
#endif
}

/* An elevated process does not receive messages from the non-elevated Explorer
   unless they are explicitly allowed (Vista+). Without this the tray icon is lost
   for good when Explorer restarts, one of the original applet's problems. */
static void AllowMessage(HWND hwnd, UINT msg)
{
    typedef BOOL (WINAPI *PFNEX)(HWND, UINT, DWORD, void *);
    typedef BOOL (WINAPI *PFN)(UINT, DWORD);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFNEX fex = (PFNEX)(void *)GetProcAddress(u, "ChangeWindowMessageFilterEx");
    PFN f;
    if (fex && fex(hwnd, msg, 1 /* MSGFLT_ALLOW */, NULL)) return;
    f = (PFN)(void *)GetProcAddress(u, "ChangeWindowMessageFilter");
    if (f) f(msg, 1 /* MSGFLT_ADD */);
}

/* ------------------------------------------------------------------------- */
/* Volume and device enumeration                                              */
/* ------------------------------------------------------------------------- */

#define MAXVOL   64
#define MAXDEV   64
#define MAXDISKS 8

typedef struct {
    WCHAR guid[64];            /* \\?\Volume{...}\ */
    WCHAR paths[160];          /* "E:" or "C:\mnt\data", comma separated */
    WCHAR letter;              /* first drive letter, or 0 */
    DWORD devType;             /* FILE_DEVICE_DISK / FILE_DEVICE_CD_ROM / ... */
    int   nDisks;
    DWORD disks[MAXDISKS];
    BOOL  system;              /* holds the Windows directory */
} VOLINFO;

typedef struct {
    int     kind;              /* K_* */
    int     number;            /* PhysicalDriveN / CdRomN, or -1 */
    BOOL    hotswap;           /* has a removable devnode at or above it */
    BOOL    system;            /* it or a device that goes away with it holds Windows */
    BOOL    ownSystem;
    BOOL    multi;             /* one of its volumes spans several disks */
    DEVINST dnSelf, dnTarget;
    WCHAR   selfId[MAX_DEVICE_ID_LEN];
    WCHAR   targetId[MAX_DEVICE_ID_LEN];
    WCHAR   name[128];
    WCHAR   pdo[64];
    WCHAR   drives[160];
} DEVENTRY;

typedef struct {
    int      nDev, nVol;
    DEVENTRY dev[MAXDEV];
    VOLINFO  vol[MAXVOL];
} DEVLIST;

static const GUID kGuidDisk  = { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
static const GUID kGuidCdrom = { 0x53f56308, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

static void AddPath(WCHAR *list, int cap, const WCHAR *p)
{
    if (list[0]) StrCat(list, cap, L", ");
    StrCat(list, cap, p);
}

static void ReadVolume(VOLINFO *v, const WCHAR *guid, const WCHAR *winVol)
{
    WCHAR names[512], dev[64];
    DWORD ret = 0;
    HANDLE h;
    STORAGE_DEVICE_NUMBER sdn;
    DWORD br;
    BOOL haveSdn;
    union {
        VOLUME_DISK_EXTENTS e;
        BYTE b[sizeof(VOLUME_DISK_EXTENTS) + 15 * sizeof(DISK_EXTENT)];
    } ext;
    int n;

    ZeroMemory(v, sizeof *v);
    StrCpy(v->guid, 64, guid);
    v->system = winVol[0] && !lstrcmpiW(guid, winVol);

    if (GetVolumePathNamesForVolumeNameW(guid, names, 510, &ret)) {
        WCHAR *p;
        names[510] = names[511] = 0;
        for (p = names; *p; p += lstrlenW(p) + 1) {
            int l = lstrlenW(p);
            if (l == 3 && p[1] == ':') {
                WCHAR d[3] = { p[0], ':', 0 };
                if (!v->letter) v->letter = p[0];
                AddPath(v->paths, 160, d);
            } else {
                WCHAR t[MAX_PATH];
                StrCpy(t, MAX_PATH, p);
                if (l > 3 && t[l - 1] == '\\') t[l - 1] = 0;
                AddPath(v->paths, 160, t);
            }
        }
    }

    /* Do not touch floppy drives: opening them makes the drive seek. */
    if ((v->letter == 'A' || v->letter == 'B') && GetDriveTypeW(guid) == DRIVE_REMOVABLE)
        return;

    StrCpy(dev, 64, guid);
    n = lstrlenW(dev);
    if (n && dev[n - 1] == '\\') dev[n - 1] = 0;
    h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    haveSdn = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof sdn, &br, NULL);
    if (haveSdn && sdn.DeviceType != FILE_DEVICE_DISK) {
        v->devType = sdn.DeviceType;
        v->disks[v->nDisks++] = sdn.DeviceNumber;
    } else if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof ext, &br, NULL)) {
        DWORD i, j;
        v->devType = FILE_DEVICE_DISK;
        for (i = 0; i < ext.e.NumberOfDiskExtents && i < 16; i++) {
            DWORD d = ext.e.Extents[i].DiskNumber;
            for (j = 0; j < (DWORD)v->nDisks && v->disks[j] != d; j++) ;
            if (j == (DWORD)v->nDisks && v->nDisks < MAXDISKS) v->disks[v->nDisks++] = d;
        }
    } else if (haveSdn) {
        v->devType = sdn.DeviceType;
        v->disks[v->nDisks++] = sdn.DeviceNumber;
    }
    CloseHandle(h);
}

static void EnumVolumes(DEVLIST *L)
{
    WCHAR name[64], winDir[MAX_PATH], winRoot[MAX_PATH], winVol[64];
    HANDLE f;

    winVol[0] = 0;
    if (GetWindowsDirectoryW(winDir, MAX_PATH) && GetVolumePathNameW(winDir, winRoot, MAX_PATH))
        if (!GetVolumeNameForVolumeMountPointW(winRoot, winVol, 64)) winVol[0] = 0;

    f = FindFirstVolumeW(name, 64);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        ReadVolume(&L->vol[L->nVol], name, winVol);
        if (L->vol[L->nVol].nDisks) L->nVol++;
    } while (L->nVol < MAXVOL && FindNextVolumeW(f, name, 64));
    FindVolumeClose(f);
}

static ULONG DevStatus(DEVINST dn)
{
    ULONG st = 0, prob = 0;
    if (CM_Get_DevNode_Status(&st, &prob, dn, 0) != CR_SUCCESS) return 0;
    return st;
}

static ULONG DevCaps(DEVINST dn)
{
    ULONG caps = 0, len = sizeof caps;
    if (CM_Get_DevNode_Registry_PropertyW(dn, CM_DRP_CAPABILITIES, NULL, &caps, &len, 0) != CR_SUCCESS)
        caps = 0;
    return caps;
}

static BOOL DevStr(DEVINST dn, ULONG prop, WCHAR *buf, int cch)
{
    ULONG len = (ULONG)(cch - 1) * sizeof(WCHAR);
    ZeroMemory(buf, cch * sizeof(WCHAR));
    if (CM_Get_DevNode_Registry_PropertyW(dn, prop, NULL, buf, &len, 0) != CR_SUCCESS) {
        buf[0] = 0;
        return FALSE;
    }
    return buf[0] != 0;
}

static void DevName(DEVINST dn, WCHAR *buf, int cch)
{
    if (!DevStr(dn, CM_DRP_FRIENDLYNAME, buf, cch) && !DevStr(dn, CM_DRP_DEVICEDESC, buf, cch))
        StrCpy(buf, cch, L"Unknown device");
}

/* Walks up from dn looking for the devnode Windows would eject (DN_REMOVABLE). */
static BOOL FindRemovable(DEVINST dn, DEVINST root, DEVINST *out)
{
    int depth;
    for (depth = 0; depth < 32 && dn != root; depth++) {
        if (DevStatus(dn) & DN_REMOVABLE) { *out = dn; return TRUE; }
        if (CM_Get_Parent(&dn, dn, 0) != CR_SUCCESS) break;
    }
    return FALSE;
}

static BOOL IsAncestorOrSelf(DEVINST anc, DEVINST dn, DEVINST root)
{
    int depth;
    for (depth = 0; depth < 32; depth++) {
        if (dn == anc) return TRUE;
        if (dn == root || CM_Get_Parent(&dn, dn, 0) != CR_SUCCESS) break;
    }
    return FALSE;
}

static BOOL VolOnDevice(const VOLINFO *v, const DEVENTRY *e)
{
    int i;
    if (e->kind == K_OTHER) return FALSE;
    if (e->kind == K_CDROM ? v->devType != FILE_DEVICE_CD_ROM : v->devType != FILE_DEVICE_DISK)
        return FALSE;
    for (i = 0; i < v->nDisks; i++)
        if (v->disks[i] == (DWORD)e->number) return TRUE;
    return FALSE;
}

static void EnumClass(DEVLIST *L, const GUID *g, BOOL cd, DEVINST root)
{
    HDEVINFO h = SetupDiGetClassDevsW(g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;
    union {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W d;
        BYTE b[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024];
    } *u;

    if (h == INVALID_HANDLE_VALUE) return;
    u = Alloc(sizeof *u);
    if (!u) { SetupDiDestroyDeviceInfoList(h); return; }

    ifd.cbSize = sizeof ifd;
    for (i = 0; L->nDev < MAXDEV && SetupDiEnumDeviceInterfaces(h, NULL, g, i, &ifd); i++) {
        SP_DEVINFO_DATA dd;
        HANDLE hd;
        STORAGE_DEVICE_NUMBER sdn;
        STORAGE_PROPERTY_QUERY q;
        BYTE desc[512];
        DWORD br;
        BOOL ok, removableMedia = FALSE;
        DEVENTRY *e;
        int v;

        dd.cbSize = sizeof dd;
        u->d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(h, &ifd, &u->d, sizeof *u, NULL, &dd)) continue;

        hd = CreateFileW(u->d.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hd == INVALID_HANDLE_VALUE) continue;
        ok = DeviceIoControl(hd, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof sdn, &br, NULL);
        ZeroMemory(&q, sizeof q);
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, desc, sizeof desc, &br, NULL)
            && br >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RemovableMedia) + 1)
            removableMedia = ((STORAGE_DEVICE_DESCRIPTOR *)desc)->RemovableMedia;
        CloseHandle(hd);
        if (!ok) continue;

        e = &L->dev[L->nDev];
        ZeroMemory(e, sizeof *e);
        e->kind = cd ? K_CDROM : (removableMedia ? K_REMOVABLE : K_FIXED);
        e->number = (int)sdn.DeviceNumber;
        e->dnSelf = dd.DevInst;
        if (CM_Get_Device_IDW(dd.DevInst, e->selfId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) continue;
        e->selfId[MAX_DEVICE_ID_LEN - 1] = 0;
        DevName(dd.DevInst, e->name, 128);
        DevStr(dd.DevInst, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, e->pdo, 64);

        e->hotswap = FindRemovable(dd.DevInst, root, &e->dnTarget);
        if (!e->hotswap) e->dnTarget = e->dnSelf;
        if (e->dnTarget == e->dnSelf) {
            StrCpy(e->targetId, MAX_DEVICE_ID_LEN, e->selfId);
        } else if (CM_Get_Device_IDW(e->dnTarget, e->targetId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
            continue;
        }
        e->targetId[MAX_DEVICE_ID_LEN - 1] = 0;

        for (v = 0; v < L->nVol; v++) {
            const VOLINFO *vi = &L->vol[v];
            if (!VolOnDevice(vi, e)) continue;
            if (vi->paths[0]) AddPath(e->drives, 160, vi->paths);
            if (vi->system) e->ownSystem = TRUE;
            if (vi->nDisks > 1) e->multi = TRUE;
        }
        L->nDev++;
    }
    Free(u);
    SetupDiDestroyDeviceInfoList(h);
}

static BOOL UsedAsTarget(const DEVLIST *L, DEVINST dn)
{
    int i;
    for (i = 0; i < L->nDev; i++)
        if (L->dev[i].kind != K_OTHER && L->dev[i].dnTarget == dn) return TRUE;
    return FALSE;
}

/* Other safely-removable devices. Walking the live tree (instead of
   CM_Get_Device_ID_List) cannot fail when devices appear or disappear
   between a size query and the list query. */
static void WalkOther(DEVLIST *L, DEVINST dn, int depth)
{
    DEVINST c;
    ULONG st;
    if (depth > 48 || L->nDev >= MAXDEV) return;
    st = DevStatus(dn);
    if ((st & DN_REMOVABLE) && (st & DN_STARTED) && !(st & DN_NO_SHOW_IN_DM)
        && !(DevCaps(dn) & (CM_DEVCAP_SURPRISEREMOVALOK | CM_DEVCAP_DOCKDEVICE))
        && !UsedAsTarget(L, dn)) {
        DEVENTRY *e = &L->dev[L->nDev];
        ZeroMemory(e, sizeof *e);
        if (CM_Get_Device_IDW(dn, e->selfId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            e->selfId[MAX_DEVICE_ID_LEN - 1] = 0;
            e->kind = K_OTHER;
            e->number = -1;
            e->hotswap = TRUE;
            e->dnSelf = e->dnTarget = dn;
            StrCpy(e->targetId, MAX_DEVICE_ID_LEN, e->selfId);
            DevName(dn, e->name, 128);
            DevStr(dn, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, e->pdo, 64);
            L->nDev++;
        }
    }
    if (CM_Get_Child(&c, dn, 0) != CR_SUCCESS) return;
    do {
        WalkOther(L, c, depth + 1);
    } while (CM_Get_Sibling(&c, c, 0) == CR_SUCCESS);
}

/* Takes a fresh snapshot of all hot-swappable devices. Nothing is cached between
   calls: every menu and every removal works from the current device tree. */
static DEVLIST *EnumDevices(void)
{
    DEVLIST *L = Alloc(sizeof *L);
    DEVINST root;
    int i, j;
    if (!L) return NULL;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return L;

    EnumVolumes(L);
    EnumClass(L, &kGuidDisk, FALSE, root);
    EnumClass(L, &kGuidCdrom, TRUE, root);
    WalkOther(L, root, 0);

    /* A device is "system" if removing its target would take the Windows volume with it. */
    for (i = 0; i < L->nDev; i++) {
        DEVENTRY *e = &L->dev[i];
        for (j = 0; j < L->nDev; j++) {
            const DEVENTRY *d = &L->dev[j];
            if (d->kind != K_OTHER && IsAncestorOrSelf(e->dnTarget, d->dnSelf, root)) {
                if (d->ownSystem) e->system = TRUE;
                if (d->multi) e->multi = TRUE;
            }
        }
    }
    return L;
}

/* "Name (DISK 1) - E:, F:" (+ PDO name when enabled). */
static void Label(const DEVENTRY *e, WCHAR *out, int cap, BOOL escapeAmp)
{
    WCHAR num[32];
    out[0] = 0;
    StrCat(out, cap, e->name);
    if (e->kind == K_CDROM) {
        wsprintfW(num, L" (CDROM %d)", e->number);
        StrCat(out, cap, num);
    } else if (e->kind != K_OTHER) {
        wsprintfW(num, L" (DISK %d)", e->number);
        StrCat(out, cap, num);
    }
    if (e->drives[0]) { StrCat(out, cap, L" - "); StrCat(out, cap, e->drives); }
    if (g_set.displayPdo && e->pdo[0]) { StrCat(out, cap, L" - "); StrCat(out, cap, e->pdo); }
    if (escapeAmp) {
        WCHAR tmp[512];
        WCHAR *p;
        int n = 0;
        for (p = out; *p && n < 510; p++) {
            if (*p == '&') tmp[n++] = '&';
            tmp[n++] = *p;
        }
        tmp[n] = 0;
        StrCpy(out, cap, tmp);
    }
}

/* ------------------------------------------------------------------------- */
/* Removal                                                                    */
/* ------------------------------------------------------------------------- */

enum { R_OK = 0, R_NOTFOUND = 1, R_FAILED = 2, R_ARGS = 3 };

typedef struct {
    int   code;
    WCHAR title[160];
    WCHAR text[1024];
} RESULT;

static void SetResult(RESULT *r, int code, int titleId, const WCHAR *titleArg, int textId, const WCHAR *textArg)
{
    r->code = code;
    Fmt1(r->title, 160, T(titleId), titleArg ? titleArg : L"");
    Fmt1(r->text, 1024, T(textId), textArg ? textArg : L"");
}

typedef struct {
    DWORD     Version;
    BOOLEAN   Persist;
    BYTE      Reserved1[3];
    DWORDLONG Attributes;
    DWORDLONG AttributesMask;
    DWORD     Reserved2[4];
} HSR_SET_DISK_ATTRIBUTES;
#define HSR_IOCTL_DISK_SET_DISK_ATTRIBUTES 0x0007C0F4
#define HSR_DISK_ATTRIBUTE_OFFLINE         1

static HANDLE OpenPhysical(int kind, int number)
{
    WCHAR path[48];
    wsprintfW(path, kind == K_CDROM ? L"\\\\.\\CdRom%d" : L"\\\\.\\PhysicalDrive%d", number);
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

/* Takes the disk offline (not persistent, so it comes back online when re-attached). Vista+. */
static void SetOffline(int number)
{
    HSR_SET_DISK_ATTRIBUTES a;
    DWORD br;
    HANDLE h;
    if (g_osMajor < 6) return;
    h = OpenPhysical(K_FIXED, number);
    if (h == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&a, sizeof a);
    a.Version = sizeof a;
    a.Persist = FALSE;
    a.Attributes = HSR_DISK_ATTRIBUTE_OFFLINE;
    a.AttributesMask = HSR_DISK_ATTRIBUTE_OFFLINE;
    DeviceIoControl(h, HSR_IOCTL_DISK_SET_DISK_ATTRIBUTES, &a, sizeof a, NULL, 0, &br, NULL);
    CloseHandle(h);
}

/* Spins the drive down: ATA STANDBY IMMEDIATE, or SCSI START STOP UNIT for
   USB/SCSI bridges that do not pass ATA commands through. */
static void Spindown(int number)
{
    ATA_PASS_THROUGH_EX apt;
    struct {
        SCSI_PASS_THROUGH spt;
        UCHAR sense[32];
    } s;
    DWORD br;
    HANDLE h = OpenPhysical(K_FIXED, number);
    if (h == INVALID_HANDLE_VALUE) return;

    ZeroMemory(&apt, sizeof apt);
    apt.Length = sizeof apt;
    apt.AtaFlags = ATA_FLAGS_DRDY_REQUIRED;
    apt.TimeOutValue = 10;
    apt.CurrentTaskFile[6] = 0xE0; /* STANDBY IMMEDIATE */
    if (!DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH, &apt, sizeof apt, &apt, sizeof apt, &br, NULL)) {
        ZeroMemory(&s, sizeof s);
        s.spt.Length = sizeof s.spt;
        s.spt.CdbLength = 6;
        s.spt.SenseInfoLength = sizeof s.sense;
        s.spt.SenseInfoOffset = offsetof(__typeof__(s), sense);
        s.spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
        s.spt.TimeOutValue = 10;
        s.spt.Cdb[0] = 0x1B;           /* START STOP UNIT, START=0 */
        DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH, &s, sizeof s, &s, sizeof s, &br, NULL);
    }
    CloseHandle(h);
}

static void VetoTypeName(PNP_VETO_TYPE v, WCHAR *out, int cap)
{
    static const WCHAR *const names[] = {
        L"unknown", L"legacy device", L"pending close", L"Windows application",
        L"Windows service", L"outstanding open", L"device", L"driver",
        L"illegal device request", L"insufficient power", L"non-disableable",
        L"legacy driver", L"insufficient rights",
    };
    StrCpy(out, cap, (v >= 0 && v < (int)(sizeof names / sizeof names[0])) ? names[v] : L"unknown");
}

/* Remembers removed devnodes so that "Scan for hardware changes" can undo
   CM_REMOVE_NO_RESTART for them. */
static void RecordRemoved(const WCHAR *id)
{
    WCHAR *m = Alloc(MSZ_CAP * sizeof(WCHAR));
    if (!m) return;
    RegGetMsz(L"Removed", m, MSZ_CAP);
    if (!MszAdd(m, MSZ_CAP, id)) { m[0] = m[1] = 0; MszAdd(m, MSZ_CAP, id); }
    RegSetMsz(L"Removed", m);
    Free(m);
}

/* Deletes the (already stopped) device node so it disappears from Device Manager.
   Only called after CM_Query_And_Remove_SubTree succeeded, so the class installer
   never has to queue the removal for the next reboot. */
static BOOL UninstallDevice(const WCHAR *id)
{
    HDEVINFO h = SetupDiCreateDeviceInfoList(NULL, NULL);
    SP_DEVINFO_DATA dd;
    SP_REMOVEDEVICE_PARAMS rp;
    SP_DEVINSTALL_PARAMS_W ip;
    BOOL ok = FALSE;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    dd.cbSize = sizeof dd;
    if (SetupDiOpenDeviceInfoW(h, id, NULL, 0, &dd)) {
        ZeroMemory(&rp, sizeof rp);
        rp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        rp.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        rp.Scope = DI_REMOVEDEVICE_GLOBAL;
        if (SetupDiSetClassInstallParamsW(h, &dd, &rp.ClassInstallHeader, sizeof rp)
            && SetupDiCallClassInstaller(DIF_REMOVE, h, &dd)) {
            ok = TRUE;
            ZeroMemory(&ip, sizeof ip);
            ip.cbSize = sizeof ip;
            if (SetupDiGetDeviceInstallParamsW(h, &dd, &ip) && (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
                ok = FALSE;
        }
    }
    SetupDiDestroyDeviceInfoList(h);
    return ok;
}

/* Finds stopped disks / optical drives stuck in "Windows is removing this device"
   (CM_PROB_WILL_BE_REMOVED) - the state the original applet leaves behind when its
   uninstall was refused - and appends their IDs to m. */
static void FindPendingRemovals(DEVINST dn, int depth, WCHAR *m, int cap)
{
    ULONG st, prob;
    DEVINST c;
    WCHAR cls[32], id[MAX_DEVICE_ID_LEN];
    if (depth > 48) return;
    if (CM_Get_DevNode_Status(&st, &prob, dn, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)
        && prob == CM_PROB_WILL_BE_REMOVED && !(st & DN_STARTED)
        && DevStr(dn, CM_DRP_CLASS, cls, 32) && (!lstrcmpiW(cls, L"DiskDrive") || !lstrcmpiW(cls, L"CDROM"))
        && CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
        id[MAX_DEVICE_ID_LEN - 1] = 0;
        MszAdd(m, cap, id);
    }
    if (CM_Get_Child(&c, dn, 0) != CR_SUCCESS) return;
    do {
        FindPendingRemovals(c, depth + 1, m, cap);
    } while (CM_Get_Sibling(&c, c, 0) == CR_SUCCESS);
}

/* ------------------------------------------------------------------------- */
/* Watch list                                                                 */
/*                                                                            */
/* A removed SATA disk that is still plugged in is re-detected and re-mounted */
/* by Windows whenever the storage bus is rescanned (another device plugged   */
/* in, a PnP scan, Disk Management...), which also spins it up. No program    */
/* can stop the controller from doing that, so the tray applet watches for    */
/* such disks and removes them again until they are physically unplugged      */
/* (their SCSI address disappears from the adapter), the system restarts, or  */
/* the user runs Scan for hardware changes.                                   */
/* Entry format: "bootMinute,port,path,target,lun|instanceId"                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    DWORD boot;
    SCSI_ADDRESS sa;
    const WCHAR *id;
} WATCH;

static DWORD BootMinute(void)
{
    typedef ULONGLONG (WINAPI *PFN)(void);
    PFN f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64");
    FILETIME ft;
    ULONGLONG now, up;
    GetSystemTimeAsFileTime(&ft);
    now = (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000;
    up = f ? f() : GetTickCount();
    return (DWORD)((now - up) / 60000);
}

static BOOL GetScsiAddress(int kind, int number, SCSI_ADDRESS *sa)
{
    WCHAR path[48];
    HANDLE h;
    DWORD br;
    BOOL ok;
    wsprintfW(path, kind == K_CDROM ? L"\\\\.\\CdRom%d" : L"\\\\.\\PhysicalDrive%d", number);
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(sa, sizeof *sa);
    ok = DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, NULL, 0, sa, sizeof *sa, &br, NULL);
    CloseHandle(h);
    return ok;
}

/* 1 = the adapter still reports the target (drive connected), 0 = gone, -1 = unknown.
   IOCTL_SCSI_GET_INQUIRY_DATA returns the adapter's cached list and does not wake the drive. */
static int LunPresent(const SCSI_ADDRESS *sa)
{
    WCHAR path[32];
    HANDLE h;
    BYTE *buf;
    DWORD br, i;
    int found = -1;
    wsprintfW(path, L"\\\\.\\Scsi%u:", sa->PortNumber);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    buf = Alloc(65536);
    if (buf && DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0, buf, 65536, &br, NULL)) {
        PSCSI_ADAPTER_BUS_INFO bi = (PSCSI_ADAPTER_BUS_INFO)buf;
        found = 0;
        for (i = 0; i < bi->NumberOfBuses && !found; i++) {
            DWORD off = bi->BusData[i].InquiryDataOffset;
            while (off && off < br) {
                PSCSI_INQUIRY_DATA d = (PSCSI_INQUIRY_DATA)(buf + off);
                if (d->PathId == sa->PathId && d->TargetId == sa->TargetId && d->Lun == sa->Lun) { found = 1; break; }
                off = d->NextInquiryDataOffset;
            }
        }
    }
    Free(buf);
    CloseHandle(h);
    return found;
}

static DWORD ParseNum(const WCHAR **p)
{
    DWORD v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (DWORD)(*(*p)++ - '0');
    if (**p == ',' || **p == '|') (*p)++;
    return v;
}

static BOOL ParseWatch(const WCHAR *s, WATCH *w)
{
    const WCHAR *p = s;
    ZeroMemory(w, sizeof *w);
    w->boot = ParseNum(&p);
    w->sa.PortNumber = (UCHAR)ParseNum(&p);
    w->sa.PathId = (UCHAR)ParseNum(&p);
    w->sa.TargetId = (UCHAR)ParseNum(&p);
    w->sa.Lun = (UCHAR)ParseNum(&p);
    if (p == s || p[-1] != '|' || !*p) return FALSE;
    w->id = p;
    return TRUE;
}

static void AddWatch(const WCHAR *id, const SCSI_ADDRESS *sa)
{
    WCHAR *m = Alloc(MSZ_CAP * sizeof(WCHAR)), *n = Alloc(MSZ_CAP * sizeof(WCHAR)), *p;
    WCHAR entry[MAX_DEVICE_ID_LEN + 64];
    WATCH w;
    if (m && n) {
        RegGetMsz(L"Watch", m, MSZ_CAP);
        for (p = m; *p; p += lstrlenW(p) + 1)   /* replace an older entry for the same device */
            if (ParseWatch(p, &w) && lstrcmpiW(w.id, id)) MszAdd(n, MSZ_CAP, p);
        wsprintfW(entry, L"%lu,%u,%u,%u,%u|", BootMinute(), sa->PortNumber, sa->PathId, sa->TargetId, sa->Lun);
        StrCat(entry, MAX_DEVICE_ID_LEN + 64, id);
        MszAdd(n, MSZ_CAP, entry);
        RegSetMsz(L"Watch", n);
    }
    Free(m);
    Free(n);
}

static void ClearWatch(void)
{
    WCHAR empty[2] = { 0, 0 };
    RegSetMsz(L"Watch", empty);
}

/* Drops stale entries; returns the instance ID of a watched disk that Windows has
   brought back (copied to out) or FALSE. */
static BOOL CheckWatch(WCHAR *out, int cap)
{
    WCHAR *m = Alloc(MSZ_CAP * sizeof(WCHAR)), *n = Alloc(MSZ_CAP * sizeof(WCHAR)), *p;
    DWORD boot = BootMinute();
    BOOL changed = FALSE, back = FALSE;
    if (!m || !n) { Free(m); Free(n); return FALSE; }
    RegGetMsz(L"Watch", m, MSZ_CAP);
    for (p = m; *p; p += lstrlenW(p) + 1) {
        WATCH w;
        DEVINST dn;
        if (!ParseWatch(p, &w) || (w.boot > boot ? w.boot - boot : boot - w.boot) > 2   /* restarted */
            || LunPresent(&w.sa) == 0) {                                               /* unplugged */
            changed = TRUE;
            continue;
        }
        MszAdd(n, MSZ_CAP, p);
        if (!back && CM_Locate_DevNodeW(&dn, (DEVINSTID_W)w.id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS
            && (DevStatus(dn) & DN_STARTED)) {
            StrCpy(out, cap, w.id);
            back = TRUE;
        }
    }
    if (changed) RegSetMsz(L"Watch", n);
    Free(m);
    Free(n);
    return back;
}

static void Rescan(void)
{
    DEVINST dn, root;
    WCHAR *m = Alloc(MSZ_CAP * sizeof(WCHAR)), *p;
    ClearWatch();
    if (m && CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        FindPendingRemovals(root, 0, m, MSZ_CAP);
        for (p = m; *p; p += lstrlenW(p) + 1)
            UninstallDevice(p);
        m[0] = m[1] = 0;
    }
    if (m) {
        RegGetMsz(L"Removed", m, MSZ_CAP);
        for (p = m; *p; p += lstrlenW(p) + 1)
            if (CM_Locate_DevNodeW(&dn, p, CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS)
                CM_Setup_DevNode(dn, CM_SETUP_DEVNODE_RESET);
        m[0] = m[1] = 0;
        RegSetMsz(L"Removed", m);
        Free(m);
    }
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
}

/* Removes the device identified by want->targetId / want->selfId. The device
   list is re-read here, so a menu that was opened before Windows re-read its
   devices can never make us act on a stale or recycled device handle. */
static void RemoveDevice(const DEVENTRY *want, RESULT *r)
{
    DEVLIST *L = EnumDevices();
    const DEVENTRY *e = NULL;
    WCHAR label[512], detail[MAX_PATH + 64], vname[MAX_PATH + 1];
    HANDLE hv[MAXVOL];
    int nh = 0, i, j, k;
    DEVINST root, dn;
    BOOL affected[MAXDEV], uninstall, haveAddr;
    SCSI_ADDRESS sa;
    CONFIGRET cr;
    PNP_VETO_TYPE veto;
    ULONG flags;

    Label(want, label, 512, FALSE);
    if (!L) {
        SetResult(r, R_FAILED, S_UNEXPECTED_TITLE, NULL, S_UNEXPECTED_TEXT, L"out of memory");
        return;
    }
    for (i = 0; i < L->nDev; i++)
        if (!lstrcmpiW(L->dev[i].targetId, want->targetId) && !lstrcmpiW(L->dev[i].selfId, want->selfId)) {
            e = &L->dev[i];
            break;
        }
    if (!e || CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        SetResult(r, R_NOTFOUND, S_PROBLEM_TITLE, label, S_X_GONE, label);
        Free(L);
        return;
    }
    Label(e, label, 512, FALSE);
    if (e->system) {
        SetResult(r, R_FAILED, S_PROBLEM_TITLE, label, S_X_SYSTEM, label);
        Free(L);
        return;
    }
    if (e->multi) {
        SetResult(r, R_FAILED, S_PROBLEM_TITLE, label, S_MULTI_DRIVES, label);
        Free(L);
        return;
    }

    /* Every disk / optical drive that goes away together with the target. */
    for (i = 0; i < L->nDev; i++)
        affected[i] = L->dev[i].kind != K_OTHER && IsAncestorOrSelf(e->dnTarget, L->dev[i].dnSelf, root);

    /* Disks / optical drives removed by themselves are uninstalled afterwards, and
       watched by their SCSI address if they sit on a SCSI/ATA adapter. */
    uninstall = e->kind != K_OTHER && !lstrcmpiW(e->targetId, e->selfId);
    haveAddr = uninstall && GetScsiAddress(e->kind, e->number, &sa);

#ifdef HSR_TEST
    /* Dry run: report what would happen, touch nothing. */
    r->code = R_OK;
    StrCpy(r->title, 160, L"DRY RUN");
    StrCpy(r->text, 1024, L"Would remove: ");
    StrCat(r->text, 1024, e->targetId);
    for (j = 0; j < L->nVol; j++)
        for (i = 0; i < L->nDev; i++)
            if (affected[i] && VolOnDevice(&L->vol[j], &L->dev[i])) {
                StrCat(r->text, 1024, L"\nWould lock volume: ");
                StrCat(r->text, 1024, L->vol[j].paths[0] ? L->vol[j].paths : L->vol[j].guid);
                break;
            }
    Free(L);
    return;
#endif

    /* Flush, lock and dismount every volume on them. Locks are retried because
       indexers, antivirus and Explorer often hold a volume for a moment. */
    for (j = 0; j < L->nVol; j++) {
        const VOLINFO *v = &L->vol[j];
        WCHAR path[64];
        HANDLE h;
        BOOL locked = FALSE;
        DWORD br, err = 0;

        for (i = 0; i < L->nDev && !(affected[i] && VolOnDevice(v, &L->dev[i])); i++) ;
        if (i == L->nDev) continue;

        if (v->letter) {
            wsprintfW(path, L"\\\\.\\%c:", v->letter);
        } else {
            int n;
            StrCpy(path, 64, v->guid);
            n = lstrlenW(path);
            if (n && path[n - 1] == '\\') path[n - 1] = 0;
        }
        h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            if (err == ERROR_NOT_READY || err == ERROR_NO_MEDIA_IN_DRIVE || err == ERROR_FILE_NOT_FOUND
                || err == ERROR_PATH_NOT_FOUND || err == ERROR_DEVICE_NOT_CONNECTED)
                continue;   /* no media / already gone */
            while (nh) CloseHandle(hv[--nh]);
            SetResult(r, R_FAILED, S_PROBLEM_TITLE, label, S_IN_USE, label);
            if (v->paths[0]) {
                StrCat(r->text, 1024, L"\n\n(");
                StrCat(r->text, 1024, v->paths);
                StrCat(r->text, 1024, L")");
            }
            Free(L);
            return;
        }
        FlushFileBuffers(h);
        for (k = 0; k < 12; k++) {
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL)) { locked = TRUE; break; }
            err = GetLastError();
            if (err == ERROR_NOT_READY || err == ERROR_NO_MEDIA_IN_DRIVE) break;
            Sleep(250);
        }
        if (!locked && err != ERROR_NOT_READY && err != ERROR_NO_MEDIA_IN_DRIVE) {
            CloseHandle(h);
            while (nh) CloseHandle(hv[--nh]);
            SetResult(r, R_FAILED, S_PROBLEM_TITLE, label, S_IN_USE, label);
            if (v->paths[0]) {
                StrCat(r->text, 1024, L"\n\n(");
                StrCat(r->text, 1024, v->paths);
                StrCat(r->text, 1024, L")");
            }
            Free(L);
            return;
        }
        if (locked) DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL);
        hv[nh++] = h;
    }

    for (i = 0; i < L->nDev; i++) {
        if (!affected[i] || L->dev[i].kind == K_CDROM) continue;
        if (g_set.offline) SetOffline(L->dev[i].number);
        if (g_set.spinDown) Spindown(L->dev[i].number);
    }

    /* Handles must be closed before the query-remove, or they veto it themselves. */
    while (nh) CloseHandle(hv[--nh]);

    /* CM_REMOVE_NO_RESTART keeps the device stopped when Windows re-enumerates
       (a device being plugged elsewhere, Device Manager scans, driver installs),
       so it cannot come back to life between "safe to remove" and the unplug. */
    dn = e->dnTarget;
    flags = CM_REMOVE_UI_NOT_OK | CM_REMOVE_NO_RESTART;
    for (k = 0; k < 3; k++) {
        veto = PNP_VetoTypeUnknown;
        vname[0] = 0;
        cr = CM_Query_And_Remove_SubTreeW(dn, &veto, vname, MAX_PATH, flags);
        if (cr == CR_INVALID_FLAG && (flags & CM_REMOVE_NO_RESTART)) { flags &= ~CM_REMOVE_NO_RESTART; continue; }
        if (cr == CR_REMOVE_VETOED && veto == PNP_VetoOutstandingOpen && k < 2) { Sleep(1000); continue; }
        break;
    }
    if (cr != CR_SUCCESS && cr != CR_REMOVE_VETOED && (DevStatus(dn) & DN_REMOVABLE)) {
        veto = PNP_VetoTypeUnknown;
        vname[0] = 0;
        cr = CM_Request_Device_EjectW(dn, &veto, vname, MAX_PATH, 0);
    }
    vname[MAX_PATH] = 0;

    if (cr == CR_SUCCESS) {
        /* Uninstalled so it disappears from Device Manager (it is already stopped,
           so no reboot is needed). */
        if (uninstall && UninstallDevice(e->targetId)) {
            if (haveAddr) AddWatch(e->targetId, &sa);
        } else {
            RecordRemoved(e->targetId);
        }
        SetResult(r, R_OK, S_SAFE_TITLE, NULL, S_SAFE_TEXT, label);
    } else if (cr == CR_REMOVE_VETOED) {
        int msg;
        switch (veto) {
        case PNP_VetoOutstandingOpen: case PNP_VetoDevice: case PNP_VetoDriver:
        case PNP_VetoPendingClose: case PNP_VetoWindowsApp: case PNP_VetoWindowsService:
            msg = S_IN_USE; break;
        case PNP_VetoNonDisableable: case PNP_VetoLegacyDevice: case PNP_VetoLegacyDriver:
        case PNP_VetoIllegalDeviceRequest:
            msg = S_NOT_STOPPABLE; break;
        case PNP_VetoInsufficientRights:
            msg = S_PRIV_TEXT; break;
        default:
            msg = S_X_VETO; break;
        }
        SetResult(r, R_FAILED, S_PROBLEM_TITLE, label, msg, label);
        VetoTypeName(veto, detail, 64);
        StrCat(r->text, 1024, L"\n\n(");
        StrCat(r->text, 1024, detail);
        if (vname[0]) { StrCat(r->text, 1024, L": "); StrCat(r->text, 1024, vname); }
        StrCat(r->text, 1024, L")");
    } else {
        WCHAR code[64];
        wsprintfW(code, T(S_X_CR_ERROR), (unsigned)cr);
        SetResult(r, R_FAILED, S_PROBLEM_TITLE, label, S_UNEXPECTED_TEXT, code);
    }
    Free(L);
}

/* ------------------------------------------------------------------------- */
/* Autostart: Task Scheduler on Vista+ (UAC blocks elevated Run entries),     */
/* HKCU Run key on XP / 2003.                                                 */
/* ------------------------------------------------------------------------- */

static const CLSID kClsidTaskScheduler = { 0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };
static const IID   kIidTaskService     = { 0x2faba4c7, 0x4da9, 0x4013, { 0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85 } };

enum { AS_QUERY, AS_ENABLE, AS_DISABLE };

static void XmlEscape(WCHAR *out, int cap, const WCHAR *s)
{
    out[0] = 0;
    for (; *s; s++) {
        switch (*s) {
        case '&': StrCat(out, cap, L"&amp;"); break;
        case '<': StrCat(out, cap, L"&lt;"); break;
        case '>': StrCat(out, cap, L"&gt;"); break;
        default: { WCHAR c[2] = { *s, 0 }; StrCat(out, cap, c); }
        }
    }
}

static BOOL TaskAutostart(int action)
{
    ITaskService *svc = NULL;
    ITaskFolder *folder = NULL;
    IRegisteredTask *task = NULL;
    VARIANT empty;
    BSTR root = NULL, name = NULL, xml = NULL;
    BOOL result = FALSE;
    HRESULT hr;

    ZeroMemory(&empty, sizeof empty);
    empty.vt = VT_EMPTY;

    hr = CoCreateInstance(&kClsidTaskScheduler, NULL, CLSCTX_INPROC_SERVER, &kIidTaskService, (void **)&svc);
    if (FAILED(hr)) return FALSE;
    if (FAILED(ITaskService_Connect(svc, empty, empty, empty, empty))) goto done;
    root = SysAllocString(L"\\");
    name = SysAllocString(APP_NAME);
    if (!root || !name || FAILED(ITaskService_GetFolder(svc, root, &folder))) goto done;

    if (action == AS_QUERY) {
        result = SUCCEEDED(ITaskFolder_GetTask(folder, name, &task));
    } else if (action == AS_DISABLE) {
        hr = ITaskFolder_DeleteTask(folder, name, 0);
        result = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    } else {
        WCHAR exe[MAX_PATH], exeXml[MAX_PATH * 2];
        WCHAR *buf = Alloc(4096 * sizeof(WCHAR));
        if (!buf) goto done;
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        exe[MAX_PATH - 1] = 0;
        XmlEscape(exeXml, MAX_PATH * 2, exe);
        StrCat(buf, 4096,
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
            L"<RegistrationInfo><Author>hotswap_re</Author>"
            L"<Description>hotswap_re notification area applet</Description></RegistrationInfo>"
            L"<Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>"
            L"<Principals><Principal id=\"Author\"><GroupId>S-1-5-32-544</GroupId>"
            L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
            L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
            L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
            L"<AllowHardTerminate>true</AllowHardTerminate>"
            L"<StartWhenAvailable>false</StartWhenAvailable>"
            L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
            L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
            L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>"
            L"<Actions Context=\"Author\"><Exec><Command>");
        StrCat(buf, 4096, exeXml);
        StrCat(buf, 4096, L"</Command></Exec></Actions></Task>");
        xml = SysAllocString(buf);
        Free(buf);
        if (!xml) goto done;
        result = SUCCEEDED(ITaskFolder_RegisterTask(folder, name, xml, TASK_CREATE_OR_UPDATE,
                                                    empty, empty, TASK_LOGON_GROUP, empty, &task));
    }
done:
    if (task) IRegisteredTask_Release(task);
    if (folder) ITaskFolder_Release(folder);
    if (svc) ITaskService_Release(svc);
    if (root) SysFreeString(root);
    if (name) SysFreeString(name);
    if (xml) SysFreeString(xml);
    return result;
}

static BOOL RunKeyAutostart(int action)
{
    HKEY k;
    BOOL result = FALSE;
    if (action == AS_QUERY) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            result = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
            RegCloseKey(k);
        }
        return result;
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    if (action == AS_ENABLE) {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        exe[MAX_PATH - 1] = 0;
        StrCpy(cmd, MAX_PATH + 4, L"\"");
        StrCat(cmd, MAX_PATH + 4, exe);
        StrCat(cmd, MAX_PATH + 4, L"\"");
        result = RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)cmd, (lstrlenW(cmd) + 1) * sizeof(WCHAR)) == ERROR_SUCCESS;
    } else {
        LONG e = RegDeleteValueW(k, APP_NAME);
        result = e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(k);
    return result;
}

static BOOL Autostart(int action)
{
    if (g_osMajor >= 6) {
        if (action != AS_QUERY) RunKeyAutostart(AS_DISABLE);
        return TaskAutostart(action);
    }
    return RunKeyAutostart(action);
}

/* ------------------------------------------------------------------------- */
/* Tray applet                                                                */
/* ------------------------------------------------------------------------- */

#define WM_TRAY       (WM_APP + 1)
#define WM_JOB_DONE   (WM_APP + 2)
#define TIMER_ADDICON    1
#define TIMER_WATCH      2   /* periodic fallback check of the watch list */
#define TIMER_WATCH_FAST 3   /* one-shot check shortly after a device change */

#define ID_SCAN        100
#define ID_AUTOSTART   101
#define ID_PDO         102
#define ID_SPINDOWN    103
#define ID_OFFLINE     104
#define ID_NOTIFY      105
#define ID_USAGE       106
#define ID_ABOUT       107
#define ID_UNINSTALL   108
#define ID_EXIT        109
#define ID_SELECTNONHS 110
#define ID_TYPE_BASE   200
#define ID_LANG_BASE   300
#define ID_DEV_BASE    1000
#define ID_NONHS_BASE  2000

static UINT  g_msgTaskbarCreated;
static BOOL  g_iconAdded;
static int   g_iconRetries;
static HICON g_icon;
static volatile LONG g_busy;
static BOOL  g_inMenu;

enum { JOB_REMOVE, JOB_SCAN, JOB_REREMOVE };
typedef struct {
    int      op;
    DEVENTRY dev;
} JOB;

static void FillNid(NOTIFYICONDATAW *nid)
{
    ZeroMemory(nid, sizeof *nid);
    nid->cbSize = NOTIFYICONDATAW_V2_SIZE;   /* the largest size Windows XP accepts */
    nid->hWnd = g_hwnd;
    nid->uID = 1;
}

static void SetTip(void)
{
    NOTIFYICONDATAW nid;
    if (!g_iconAdded) return;
    FillNid(&nid);
    nid.uFlags = NIF_TIP;
    StrCpy(nid.szTip, 64, T(S_TITLE));
    if (g_busy) { StrCat(nid.szTip, 64, L" - "); StrCat(nid.szTip, 64, T(S_X_WORKING)); }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static BOOL AddIcon(void)
{
    NOTIFYICONDATAW nid;
    FillNid(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);   /* in case Explorer still has a stale one */
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    StrCpy(nid.szTip, 64, T(S_TITLE));
    g_iconAdded = Shell_NotifyIconW(NIM_ADD, &nid);
    return g_iconAdded;
}

static void Balloon(const WCHAR *title, const WCHAR *text)
{
    NOTIFYICONDATAW nid;
    if (!g_iconAdded || g_set.noBalloon) return;
    FillNid(&nid);
    nid.uFlags = NIF_INFO;
    StrCpy(nid.szInfoTitle, 64, title);
    StrCpy(nid.szInfo, 256, text);
    nid.uTimeout = 10000;
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static DWORD WINAPI JobThread(LPVOID p)
{
    JOB *job = p;
    RESULT *r = Alloc(sizeof *r);
    if (r) {
        if (job->op == JOB_SCAN) {
            Rescan();
            r->code = R_OK;
        } else {
            RemoveDevice(&job->dev, r);
        }
    }
    PostMessageW(g_hwnd, WM_JOB_DONE, (WPARAM)job->op, (LPARAM)r);
    Free(job);
    return 0;
}

static void StartJob(int op, const DEVENTRY *dev)
{
    JOB *job;
    HANDLE th;
    if (InterlockedExchange(&g_busy, 1)) {
        if (op != JOB_REREMOVE) Msg(T(S_X_BUSY), T(S_TITLE), MB_ICONINFORMATION);
        return;
    }
    job = Alloc(sizeof *job);
    if (!job) { g_busy = 0; return; }
    job->op = op;
    if (dev) job->dev = *dev;
    SetTip();
    th = CreateThread(NULL, 0, JobThread, job, 0, NULL);
    if (!th) {
        Free(job);
        g_busy = 0;
        SetTip();
        return;
    }
    CloseHandle(th);
}

static void OnJobDone(int op, RESULT *r)
{
    g_busy = 0;
    SetTip();
    if (!r) return;
    if (op == JOB_REMOVE) {
        if (r->code == R_OK) {
            if (g_iconAdded && !g_set.noBalloon)
                Balloon(r->title, r->text);
        } else {
            Msg(r->text, r->title, MB_ICONWARNING);
        }
    } else if (op == JOB_SCAN) {
        Balloon(T(S_TITLE), T(S_X_SCAN_DONE));
    }
    /* JOB_REREMOVE is silent; if it failed (volume briefly in use), the next tick retries. */
    Free(r);
    SetTimer(g_hwnd, TIMER_WATCH_FAST, 300, NULL);
}

/* Re-removes a watched disk that a storage bus rescan brought back. */
static void WatchTick(void)
{
    DEVENTRY d;
    if (g_busy) return;
    ZeroMemory(&d, sizeof d);
    if (!CheckWatch(d.selfId, MAX_DEVICE_ID_LEN)) return;
    StrCpy(d.targetId, MAX_DEVICE_ID_LEN, d.selfId);
    StrCpy(d.name, 128, d.selfId);
    d.number = -1;
    StartJob(JOB_REREMOVE, &d);
}

static int TrackMenu(HMENU m)
{
    POINT pt;
    int cmd;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    g_inMenu = TRUE;
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    g_inMenu = FALSE;
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    return cmd;
}

static BOOL ShownInMain(const DEVENTRY *e)
{
    if (e->system || !(g_set.dflags & (1u << e->kind))) return FALSE;
    return e->hotswap || (e->kind != K_OTHER && MszHas(g_nonHs, e->selfId));
}

static void DeviceMenu(void)
{
    DEVLIST *L = EnumDevices();
    HMENU m = CreatePopupMenu();
    WCHAR label[512], item[1024];
    int i, cmd, shown = 0;
    if (!L || !m) { Free(L); if (m) DestroyMenu(m); return; }

    if (g_busy) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_X_BUSY));
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    for (i = 0; i < L->nDev; i++) {
        const DEVENTRY *e = &L->dev[i];
        if (!ShownInMain(e)) continue;
        Label(e, label, 512, TRUE);
        Fmt1(item, 1024, T(S_M_SAFELY), label);
        AppendMenuW(m, MF_STRING | (g_busy ? MF_GRAYED : 0), ID_DEV_BASE + i, item);
        shown++;
    }
    if (!shown) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_M_NO_DEVICES));

    cmd = TrackMenu(m);
    DestroyMenu(m);
    if (cmd >= ID_DEV_BASE && cmd < ID_DEV_BASE + L->nDev)
        StartJob(JOB_REMOVE, &L->dev[cmd - ID_DEV_BASE]);
    Free(L);
}

static void NonHotswapMenu(void)
{
    DEVLIST *L = EnumDevices();
    HMENU m = CreatePopupMenu();
    WCHAR label[512];
    int i, cmd, shown = 0;
    if (!L || !m) { Free(L); if (m) DestroyMenu(m); return; }

    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_M_NONHS_HDR));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < L->nDev; i++) {
        const DEVENTRY *e = &L->dev[i];
        if (e->hotswap || e->kind == K_OTHER) continue;
        Label(e, label, 512, TRUE);
        if (e->system) StrCat(label, 512, T(S_X_SYSTEM_TAG));
        AppendMenuW(m, MF_STRING | (e->system ? MF_GRAYED : 0) | (MszHas(g_nonHs, e->selfId) ? MF_CHECKED : 0),
                    ID_NONHS_BASE + i, label);
        shown++;
    }
    if (!shown) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_M_NO_DEVICES));

    cmd = TrackMenu(m);
    DestroyMenu(m);
    if (cmd >= ID_NONHS_BASE && cmd < ID_NONHS_BASE + L->nDev) {
        const WCHAR *id = L->dev[cmd - ID_NONHS_BASE].selfId;
        if (MszHas(g_nonHs, id)) MszDel(g_nonHs, id);
        else MszAdd(g_nonHs, MSZ_CAP, id);
        RegSetMsz(L"NonHotSwap", g_nonHs);
    }
    Free(L);
}

static void ShowUsage(void)
{
    WCHAR *buf = Alloc(4096 * sizeof(WCHAR));
    int n;
    if (!buf) return;
    StrCat(buf, 4096, T(S_USAGE_TEXT));
    for (n = lstrlenW(buf); n && buf[n - 1] == '\n'; n--) buf[n - 1] = 0;
    StrCat(buf, 4096, L"\n");
    StrCat(buf, 4096, T(S_X_USAGE_EXTRA));
    Msg(buf, T(S_HOWTO_TITLE), MB_ICONINFORMATION);
    Free(buf);
}

static void Uninstall(void)
{
    if (Msg(T(S_UNINST_CONFIRM), T(S_UNINST_TITLE), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    Autostart(AS_DISABLE);
    RegDeleteKeyW(HKEY_CURRENT_USER, REG_KEY);
    Msg(T(S_UNINSTALLED_TEXT), T(S_UNINSTALLED_TITLE), MB_ICONINFORMATION);
    DestroyWindow(g_hwnd);
}

static void ConfigMenu(void)
{
    HMENU m = CreatePopupMenu(), types = CreatePopupMenu(), langs = CreatePopupMenu();
    BOOL autostart = Autostart(AS_QUERY);
    static const int typeStr[K_COUNT] = { S_T_FIXED, S_T_REMOVABLE, S_T_CDROM, S_T_SAFELY };
    int i, cmd;
    if (!m || !types || !langs) {
        if (m) DestroyMenu(m);
        if (types) DestroyMenu(types);
        if (langs) DestroyMenu(langs);
        return;
    }

    for (i = 0; i < K_COUNT; i++)
        AppendMenuW(types, MF_STRING | ((g_set.dflags & (1u << i)) ? MF_CHECKED : 0), ID_TYPE_BASE + i, T(typeStr[i]));
    for (i = 0; i < NLANG; i++)
        AppendMenuW(langs, MF_STRING, ID_LANG_BASE + i, g_langNames[i]);
    CheckMenuRadioItem(langs, ID_LANG_BASE, ID_LANG_BASE + NLANG - 1, ID_LANG_BASE + g_lang, MF_BYCOMMAND);

    AppendMenuW(m, MF_STRING | (g_busy ? MF_GRAYED : 0), ID_SCAN, T(S_M_SCAN));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)types, T(S_M_DEVTYPE));
    AppendMenuW(m, MF_STRING, ID_SELECTNONHS, T(S_X_SELECT_NONHS_DOTS));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)langs, T(S_M_LANGUAGE));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart ? MF_CHECKED : 0), ID_AUTOSTART, T(S_M_AUTOSTART));
    AppendMenuW(m, MF_STRING | (g_set.displayPdo ? MF_CHECKED : 0), ID_PDO, T(S_M_PDO));
    AppendMenuW(m, MF_STRING | (g_set.spinDown ? MF_CHECKED : 0), ID_SPINDOWN, T(S_M_SPINDOWN));
    if (g_osMajor >= 6)
        AppendMenuW(m, MF_STRING | (g_set.offline ? MF_CHECKED : 0), ID_OFFLINE, T(S_M_OFFLINE));
    else
        AppendMenuW(m, MF_STRING | MF_GRAYED, ID_OFFLINE, T(S_X_OFFLINE_NA));
    AppendMenuW(m, MF_STRING | (g_set.noBalloon ? 0 : MF_CHECKED), ID_NOTIFY, T(S_X_NOTIFY));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_USAGE, T(S_M_USAGE));
    AppendMenuW(m, MF_STRING, ID_ABOUT, T(S_M_ABOUT));
    AppendMenuW(m, MF_STRING, ID_UNINSTALL, T(S_M_UNINSTALL));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, T(S_M_EXIT));

    cmd = TrackMenu(m);
    DestroyMenu(m);   /* also destroys the attached submenus */

    if (cmd >= ID_TYPE_BASE && cmd < ID_TYPE_BASE + K_COUNT) {
        g_set.dflags ^= 1u << (cmd - ID_TYPE_BASE);
        RegSetDword(L"DFlags", g_set.dflags);
    } else if (cmd >= ID_LANG_BASE && cmd < ID_LANG_BASE + NLANG) {
        g_lang = cmd - ID_LANG_BASE;
        RegSetDword(L"Language", (DWORD)g_lang);
        SetTip();
    } else switch (cmd) {
    case ID_SCAN:        StartJob(JOB_SCAN, NULL); break;
    case ID_SELECTNONHS: NonHotswapMenu(); break;
    case ID_AUTOSTART:
        if (!Autostart(autostart ? AS_DISABLE : AS_ENABLE)) {
            WCHAR code[32];
            wsprintfW(code, L"%lu", GetLastError());
            {
                WCHAR text[256];
                Fmt1(text, 256, T(S_UNEXPECTED_TEXT), code);
                Msg(text, T(S_UNEXPECTED_TITLE), MB_ICONWARNING);
            }
        }
        break;
    case ID_PDO:      g_set.displayPdo ^= 1; RegSetDword(L"DisplayPDO", g_set.displayPdo); break;
    case ID_SPINDOWN: g_set.spinDown ^= 1;   RegSetDword(L"SpinDown", g_set.spinDown); break;
    case ID_OFFLINE:  g_set.offline ^= 1;    RegSetDword(L"Offline", g_set.offline); break;
    case ID_NOTIFY:   g_set.noBalloon ^= 1;  RegSetDword(L"NoBalloon", g_set.noBalloon); break;
    case ID_USAGE:    ShowUsage(); break;
    case ID_ABOUT:    Msg(T(S_X_ABOUT_TEXT), T(S_ABOUT_TITLE), MB_ICONINFORMATION); break;
    case ID_UNINSTALL: Uninstall(); break;
    case ID_EXIT:     DestroyWindow(g_hwnd); break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        AddIcon();
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (g_inMenu) return 0;
        if (lp == WM_LBUTTONUP) {
            if (GetKeyState(VK_CONTROL) < 0) NonHotswapMenu();
            else DeviceMenu();
        } else if (lp == WM_RBUTTONUP) {
            ConfigMenu();
        }
        return 0;
    case WM_JOB_DONE:
        OnJobDone((int)wp, (RESULT *)lp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ADDICON && (AddIcon() || ++g_iconRetries > 120))
            KillTimer(hwnd, TIMER_ADDICON);
        if (wp == TIMER_WATCH_FAST) KillTimer(hwnd, TIMER_WATCH_FAST);
        if (wp == TIMER_WATCH || wp == TIMER_WATCH_FAST) WatchTick();
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVNODES_CHANGED)
            SetTimer(hwnd, TIMER_WATCH_FAST, 300, NULL);
        return TRUE;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid;
        FillNid(&nid);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_iconAdded = FALSE;
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunTray(void)
{
    WNDCLASSW wc;
    MSG msg;
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"hotswap_re Applet Mutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    if (g_osMajor >= 6) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                             RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0, NULL);
        /* Keep an existing autostart task pointing at this executable after it was moved. */
        if (Autostart(AS_QUERY)) Autostart(AS_ENABLE);
    }

    g_icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.hIcon = g_icon;
    wc.lpszClassName = L"hotswap_re Applet Window Class";
    if (!RegisterClassW(&wc)) return 1;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"hotswap_re Applet", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
    if (!g_hwnd) return 1;

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_msgTaskbarCreated) AllowMessage(g_hwnd, g_msgTaskbarCreated);
    AllowMessage(g_hwnd, WM_TRAY);

    /* At logon the notification area may not exist yet: keep retrying. */
    if (!AddIcon()) SetTimer(g_hwnd, TIMER_ADDICON, 1000, NULL);

    /* Disk arrivals (a removed disk brought back by a bus rescan) wake the watcher at once;
       DBT_DEVNODES_CHANGED is broadcast to top-level windows anyway, and a slow timer is the fallback. */
    {
        DEV_BROADCAST_DEVICEINTERFACE_W f;
        ZeroMemory(&f, sizeof f);
        f.dbcc_size = sizeof f;
        f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        f.dbcc_classguid = kGuidDisk;
        RegisterDeviceNotificationW(g_hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    SetTimer(g_hwnd, TIMER_WATCH, 2000, NULL);
    SetTimer(g_hwnd, TIMER_WATCH_FAST, 300, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_osMajor >= 6) CoUninitialize();
    if (g_icon) DestroyIcon(g_icon);
    if (mutex) CloseHandle(mutex);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Command line: hotswap_re [drive letter | path | PDO | DISK=# [-Q] | -S]    */
/* ------------------------------------------------------------------------- */

static const DEVENTRY *FindByVolume(const DEVLIST *L, const WCHAR *path)
{
    WCHAR full[MAX_PATH], root[MAX_PATH], guid[64];
    int i, j;
    StrCpy(full, MAX_PATH - 1, path);
    if (lstrlenW(full) == 2 && full[1] == ':') StrCat(full, MAX_PATH, L"\\");
    if (!GetVolumePathNameW(full, root, MAX_PATH)) return NULL;
    if (!GetVolumeNameForVolumeMountPointW(root, guid, 64)) return NULL;
    for (j = 0; j < L->nVol; j++) {
        if (lstrcmpiW(L->vol[j].guid, guid)) continue;
        for (i = 0; i < L->nDev; i++)
            if (VolOnDevice(&L->vol[j], &L->dev[i])) return &L->dev[i];
    }
    return NULL;
}

static const DEVENTRY *FindTarget(const DEVLIST *L, const WCHAR *arg)
{
    int i, n = lstrlenW(arg);
    if (StartsWithI(arg, L"DISK=")) {
        int num = ParseInt(arg + 5);
        for (i = 0; i < L->nDev; i++)
            if ((L->dev[i].kind == K_FIXED || L->dev[i].kind == K_REMOVABLE) && L->dev[i].number == num)
                return &L->dev[i];
        return NULL;
    }
    if (StartsWithI(arg, L"\\Device\\")) {
        WCHAR pdo[64];
        for (i = 0; i < L->nDev; i++) {
            if (!lstrcmpiW(L->dev[i].pdo, arg)) return &L->dev[i];
            if (DevStr(L->dev[i].dnTarget, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, pdo, 64) && !lstrcmpiW(pdo, arg))
                return &L->dev[i];
        }
        return NULL;
    }
    if (n == 1 && ((arg[0] >= 'A' && arg[0] <= 'Z') || (arg[0] >= 'a' && arg[0] <= 'z'))) {
        WCHAR d[4] = { arg[0], ':', '\\', 0 };
        return FindByVolume(L, d);
    }
    return FindByVolume(L, arg);
}

static void ListDevices(void)
{
    static const WCHAR *const kinds[K_COUNT] = { L"fixed", L"removable", L"optical", L"other" };
    DEVLIST *L = EnumDevices();
    WCHAR *buf = Alloc(16384 * sizeof(WCHAR)), label[512], line[MAX_DEVICE_ID_LEN * 2 + 64];
    int i;
    if (L && buf) {
        DWORD save = g_set.displayPdo;
        g_set.displayPdo = TRUE;
        for (i = 0; i < L->nDev; i++) {
            const DEVENTRY *e = &L->dev[i];
            Label(e, label, 512, FALSE);
            StrCat(buf, 16384, label);
            wsprintfW(line, L"\n    [%s%s%s%s]\n    %s\n",
                      kinds[e->kind], e->hotswap ? L", hotswap" : L"",
                      MszHas(g_nonHs, e->selfId) ? L", user-selected" : L"",
                      e->system ? L", system" : L"", e->selfId);
            StrCat(buf, 16384, line);
            if (lstrcmpiW(e->selfId, e->targetId)) {
                wsprintfW(line, L"    removed via: %s\n", e->targetId);
                StrCat(buf, 16384, line);
            }
        }
        g_set.displayPdo = save;
        if (!L->nDev) StrCat(buf, 16384, T(S_M_NO_DEVICES));
        Msg(buf, T(S_X_LIST_TITLE), MB_ICONINFORMATION);
    }
    Free(buf);
    Free(L);
}

static int RunCommandLine(int argc, WCHAR **argv)
{
    const WCHAR *target = NULL;
    BOOL scan = FALSE;
    int i, code = R_OK;

    for (i = 1; i < argc; i++) {
        const WCHAR *a = argv[i];
        if ((a[0] == '-' || a[0] == '/') && a[1] && !a[2]) {
            switch (a[1]) {
            case 'q': case 'Q': g_quiet = TRUE; continue;
            case 's': case 'S': scan = TRUE; continue;
            case '?': case 'h': case 'H': ShowUsage(); return R_OK;
            case 'l': case 'L': ListDevices(); return R_OK;
            }
        }
        if (target) { if (!g_quiet) ShowUsage(); return R_ARGS; }
        target = a;
    }

    if (target) {
        DEVLIST *L = EnumDevices();
        const DEVENTRY *e = L ? FindTarget(L, target) : NULL;
        RESULT *r = Alloc(sizeof *r);
        if (!r) { Free(L); return R_FAILED; }
        if (!e) {
            SetResult(r, R_NOTFOUND, S_PROBLEM_TITLE, target, S_X_NOTFOUND, target);
        } else {
            DEVENTRY copy = *e;
            RemoveDevice(&copy, r);
        }
        code = r->code;
        if (!g_quiet)
            Msg(r->text, r->title, code == R_OK ? MB_ICONINFORMATION : MB_ICONWARNING);
        Free(r);
        Free(L);
    }
    if (scan) {
        Rescan();
        if (!target && !g_quiet) Msg(T(S_X_SCAN_DONE), T(S_TITLE), MB_ICONINFORMATION);
    }
    return code;
}

/* ------------------------------------------------------------------------- */
/* Entry point (no C runtime)                                                 */
/* ------------------------------------------------------------------------- */

static int Main(void)
{
    OSVERSIONINFOW vi;
    WCHAR **argv;
    int argc = 0, rc;

    g_hInst = GetModuleHandleW(NULL);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    ZeroMemory(&vi, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    GetVersionExW(&vi);
    g_osMajor = vi.dwMajorVersion;

    LoadSettings();
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (rc = 1; argv && rc < argc; rc++)
        if (!lstrcmpiW(argv[rc], L"-Q") || !lstrcmpiW(argv[rc], L"/Q")) g_quiet = TRUE;

    if (IsWow64()) {
        if (!g_quiet) Msg(T(S_X_WOW64), T(S_TITLE), MB_ICONERROR);
        return R_ARGS;
    }
#ifndef HSR_TEST
    if (!IsAdmin()) {
        if (!g_quiet) Msg(T(S_PRIV_TEXT), T(S_PRIV_TITLE), MB_ICONERROR);
        return R_ARGS;
    }
#endif

    if (argv && argc > 1) rc = RunCommandLine(argc, argv);
    else rc = RunTray();
    if (argv) LocalFree(argv);
    return rc;
}

void __cdecl entry(void)
{
    ExitProcess((UINT)Main());
}
