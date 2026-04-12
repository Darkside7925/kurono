#include "pkgmgr.h"
#include "../shell/shell.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"

//  package manager implementation

Package PackageManager::packages[PKG_MAX_PACKAGES];
int PackageManager::package_count = 0;

static int plen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void pcpy(char* d, const char* s, int m) { int i=0; while (s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0; }
static bool peq(const char* a, const char* b) { while (*a&&*b) { if(*a!=*b) return false; a++; b++; } return *a==*b; }
static int pa(char* b, int p, int m, const char* s) { while (*s&&p<m-1) b[p++]=*s++; b[p]=0; return p; }
static int pac(char* b, int p, int m, char c) { if (p<m-1) {b[p++]=c; b[p]=0;} return p; }
static int pai(char* b, int p, int m, unsigned int v) {
    if (v==0) return pac(b,p,m,'0');
    char t[12]; int ti=0; while (v>0) { t[ti++]='0'+(v%10); v/=10; } while (ti>0) p=pac(b,p,m,t[--ti]); return p;
}

static bool pcontains(const char* haystack, const char* needle) {
    int hl = plen(haystack), nl = plen(needle);
    for (int i = 0; i <= hl - nl; i++) {
        bool match = true;
        for (int j = 0; j < nl; j++) {
            char a = haystack[i+j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static void add_pkg(const char* name, const char* ver, const char* desc, const char* cat, PkgState state, unsigned int sz) {
    Package* p = &PackageManager::GetPackages()[PackageManager::GetPackageCount()];
    // we need to use the static member directly
    (void)p; // handled below
}

void PackageManager::AddDefaultPackages() {
    auto add = [](const char* name, const char* ver, const char* desc, const char* cat, PkgState st, unsigned int sz) {
        if (package_count >= PKG_MAX_PACKAGES) return;
        Package& p = packages[package_count++];
        pcpy(p.name, name, PKG_MAX_NAME);
        pcpy(p.version, ver, 16);
        pcpy(p.description, desc, PKG_MAX_DESC);
        pcpy(p.category, cat, 16);
        p.state = st;
        p.size = sz;
        p.dep_count = 0;
    };

    // core packages (pre-installed)
    add("kurono-kernel",    "1.0.0", "Kurono hybrid kernel",           "core",     PKG_INSTALLED, 2048);
    add("kurono-shell",     "1.0.0", "Kurono command shell (ksh)",     "core",     PKG_INSTALLED, 128);
    add("kurono-desktop",   "1.0.0", "Desktop environment",            "core",     PKG_INSTALLED, 512);
    add("kvfs",             "1.0.0", "Virtual filesystem",             "core",     PKG_INSTALLED, 64);
    add("kcl",              "1.0.0", "Kurono Command Language",        "core",     PKG_INSTALLED, 96);
    add("supr-security",    "1.0.0", "SUPR security engine",           "core",     PKG_INSTALLED, 48);
    add("linux-bridge",     "1.0.0", "Linux command compatibility",    "compat",   PKG_INSTALLED, 128);
    add("windows-bridge",   "1.0.0", "Windows command compatibility",  "compat",   PKG_INSTALLED, 128);

    // drivers (pre-installed)
    add("bga-driver",       "1.0.0", "Bochs Graphics Adapter driver",  "drivers",  PKG_INSTALLED, 32);
    add("ps2-keyboard",     "1.0.0", "PS/2 keyboard driver",           "drivers",  PKG_INSTALLED, 16);
    add("ps2-mouse",        "1.0.0", "PS/2 mouse driver",              "drivers",  PKG_INSTALLED, 16);
    add("pit-timer",        "1.0.0", "PIT timer driver (1kHz)",        "drivers",  PKG_INSTALLED, 8);
    add("rtc-driver",       "1.0.0", "CMOS RTC driver",                "drivers",  PKG_INSTALLED, 8);
    add("serial-driver",    "1.0.0", "COM1 serial driver",             "drivers",  PKG_INSTALLED, 8);

    // apps (pre-installed)
    add("calculator",       "1.0.0", "Calculator application",         "apps",     PKG_INSTALLED, 32);
    add("file-browser",     "1.0.0", "File browser application",       "apps",     PKG_INSTALLED, 48);
    add("terminal",         "1.0.0", "Terminal emulator",              "apps",     PKG_INSTALLED, 64);
    add("text-editor",      "1.0.0", "Basic text editor",             "apps",     PKG_INSTALLED, 48);
    add("settings",         "1.0.0", "System settings",               "apps",     PKG_INSTALLED, 32);
    add("task-manager",     "1.0.0", "Process monitor",               "apps",     PKG_INSTALLED, 32);

    // available (not yet installed)
    add("wifi-driver",      "1.0.0", "WiFi network driver",           "drivers",  PKG_AVAILABLE, 64);
    add("ethernet-driver",  "1.0.0", "Ethernet NIC driver",           "drivers",  PKG_AVAILABLE, 48);
    add("usb-driver",       "1.2.0", "USB host controller driver",    "drivers",  PKG_AVAILABLE, 96);
    add("audio-driver",     "1.0.0", "AC97/HDA audio driver",         "drivers",  PKG_AVAILABLE, 64);
    add("ahci-driver",      "1.0.0", "AHCI SATA driver",             "drivers",  PKG_AVAILABLE, 48);
    add("nvme-driver",      "1.0.0", "NVMe storage driver",          "drivers",  PKG_AVAILABLE, 48);
    add("stb-image",        "2.28.0","Image decoding library",        "libs",     PKG_INSTALLED, 128);
    add("stb-truetype",     "1.26.0","TrueType font renderer",        "libs",     PKG_INSTALLED, 96);
    add("tcp-stack",        "1.0.0", "TCP/IP network stack",          "network",  PKG_AVAILABLE, 128);
    add("dns-resolver",     "1.0.0", "DNS resolution service",        "network",  PKG_AVAILABLE, 32);
    add("http-client",      "1.0.0", "HTTP/HTTPS client",             "network",  PKG_AVAILABLE, 64);
    add("ssh-client",       "1.0.0", "SSH remote client",             "network",  PKG_AVAILABLE, 96);
    add("image-viewer",     "1.0.0", "PNG/JPEG viewer",               "apps",     PKG_AVAILABLE, 32);
    add("music-player",     "1.0.0", "Audio player",                  "apps",     PKG_AVAILABLE, 48);
    add("web-browser",      "0.1.0", "Minimal web browser",           "apps",     PKG_AVAILABLE, 256);
    add("games-pack",       "1.0.0", "Tetris, Snake, Minesweeper",    "apps",     PKG_AVAILABLE, 64);
}

void PackageManager::Init() {
    package_count = 0;
    AddDefaultPackages();
    SerialLogger::Log("PkgMgr: Initialized with ");
    SerialLogger::Log("packages\r\n");
}

bool PackageManager::Install(const char* name) {
    Package* p = Find(name);
    if (!p) return false;
    if (p->state == PKG_INSTALLED) return true;

    // install dependencies first
    for (int i = 0; i < p->dep_count; i++) {
        Install(p->deps[i]);
    }

    p->state = PKG_INSTALLED;

    // create package record in filesystem
    char path[128];
    pcpy(path, "/kurono/packages/", 128);
    int pl = plen(path);
    pcpy(path + pl, name, 128 - pl);
    KVFS::Mkdirs(path);

    return true;
}

bool PackageManager::Remove(const char* name) {
    Package* p = Find(name);
    if (!p) return false;
    if (p->state != PKG_INSTALLED) return false;

    // don't allow removing core packages
    if (peq(p->category, "core")) return false;

    p->state = PKG_AVAILABLE;
    return true;
}

bool PackageManager::Update(const char* name) {
    Package* p = Find(name);
    if (!p || p->state != PKG_INSTALLED) return false;
    p->state = PKG_UPDATING;
    // simulate update...
    p->state = PKG_INSTALLED;
    return true;
}

bool PackageManager::UpdateAll() {
    bool any = false;
    for (int i = 0; i < package_count; i++) {
        if (packages[i].state == PKG_INSTALLED) {
            Update(packages[i].name);
            any = true;
        }
    }
    return any;
}

Package* PackageManager::Find(const char* name) {
    for (int i = 0; i < package_count; i++) {
        if (peq(packages[i].name, name)) return &packages[i];
    }
    return nullptr;
}

int PackageManager::Search(const char* pattern, Package** results, int max_results) {
    int count = 0;
    for (int i = 0; i < package_count && count < max_results; i++) {
        if (pcontains(packages[i].name, pattern) || pcontains(packages[i].description, pattern)) {
            results[count++] = &packages[i];
        }
    }
    return count;
}

int PackageManager::ListInstalled(Package** results, int max_results) {
    int count = 0;
    for (int i = 0; i < package_count && count < max_results; i++) {
        if (packages[i].state == PKG_INSTALLED) results[count++] = &packages[i];
    }
    return count;
}

int PackageManager::ListAll(Package** results, int max_results) {
    int count = 0;
    for (int i = 0; i < package_count && count < max_results; i++) {
        results[count++] = &packages[i];
    }
    return count;
}

Package* PackageManager::GetPackages() { return packages; }
int PackageManager::GetPackageCount() { return package_count; }

//  shell commands

void PackageManager::RegisterCommands(void* shell_ptr) {
    KuronoShell* sh = (KuronoShell*)shell_ptr;
    sh->RegisterCommand("kpkg",    "Package manager",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_install));
    sh->RegisterCommand("install", "Install package",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_install));
    sh->RegisterCommand("remove",  "Remove package",     ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_remove));
    sh->RegisterCommand("update",  "Update packages",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_update));
    sh->RegisterCommand("search",  "Search packages",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_search));
    sh->RegisterCommand("list",    "List packages",      ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_list));
    sh->RegisterCommand("pkginfo", "Package info",       ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_info));
}

int PackageManager::cmd_install(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: install <package>\n");
    const char* name = argv[1];
    if (peq(argv[0], "kpkg") && argc >= 3) name = argv[2];

    int p = pa(out, 0, mx, "Installing ");
    p = pa(out, p, mx, name);
    p = pa(out, p, mx, "...\n");

    if (Install(name)) {
        Package* pkg = Find(name);
        p = pa(out, p, mx, "✓ ");
        p = pa(out, p, mx, name);
        p = pa(out, p, mx, " ");
        if (pkg) p = pa(out, p, mx, pkg->version);
        p = pa(out, p, mx, " installed successfully (");
        if (pkg) p = pai(out, p, mx, pkg->size);
        p = pa(out, p, mx, " KB)\n");
    } else {
        p = pa(out, p, mx, "✗ Package not found: ");
        p = pa(out, p, mx, name);
        p = pac(out, p, mx, '\n');
    }
    return p;
}

int PackageManager::cmd_remove(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: remove <package>\n");

    int p = 0;
    if (Remove(argv[1])) {
        p = pa(out, p, mx, "✓ ");
        p = pa(out, p, mx, argv[1]);
        p = pa(out, p, mx, " removed.\n");
    } else {
        p = pa(out, p, mx, "✗ Cannot remove: ");
        p = pa(out, p, mx, argv[1]);
        p = pac(out, p, mx, '\n');
    }
    return p;
}

int PackageManager::cmd_update(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;
    if (argc >= 2) {
        if (Update(argv[1])) {
            p = pa(out, p, mx, "✓ ");
            p = pa(out, p, mx, argv[1]);
            p = pa(out, p, mx, " updated.\n");
        } else {
            p = pa(out, p, mx, "✗ Cannot update: ");
            p = pa(out, p, mx, argv[1]);
            p = pac(out, p, mx, '\n');
        }
    } else {
        p = pa(out, p, mx, "Updating all packages...\n");
        UpdateAll();
        p = pa(out, p, mx, "✓ All packages up to date.\n");
    }
    return p;
}

int PackageManager::cmd_search(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: search <term>\n");

    Package* results[32];
    int count = Search(argv[1], results, 32);

    int p = 0;
    if (count == 0) {
        p = pa(out, p, mx, "No packages found matching '");
        p = pa(out, p, mx, argv[1]);
        p = pa(out, p, mx, "'\n");
    } else {
        for (int i = 0; i < count; i++) {
            p = pa(out, p, mx, results[i]->state == PKG_INSTALLED ? " [✓] " : " [ ] ");
            p = pa(out, p, mx, results[i]->name);
            int nl = plen(results[i]->name);
            for (int j = nl; j < 22; j++) p = pac(out, p, mx, ' ');
            p = pa(out, p, mx, results[i]->version);
            p = pa(out, p, mx, "  ");
            p = pa(out, p, mx, results[i]->description);
            p = pac(out, p, mx, '\n');
        }
    }
    return p;
}

int PackageManager::cmd_list(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    bool installed_only = true;
    if (argc >= 2 && peq(argv[1], "--all")) installed_only = false;

    int p = 0;
    p = pa(out, p, mx, "╔═══════════════════════════════════════════════════════╗\n");
    p = pa(out, p, mx, installed_only ? "║            Installed Packages                        ║\n"
                                       : "║            All Packages                              ║\n");
    p = pa(out, p, mx, "╚═══════════════════════════════════════════════════════╝\n\n");

    for (int i = 0; i < package_count; i++) {
        if (installed_only && packages[i].state != PKG_INSTALLED) continue;

        p = pa(out, p, mx, packages[i].state == PKG_INSTALLED ? " ● " : " ○ ");
        p = pa(out, p, mx, packages[i].name);
        int nl = plen(packages[i].name);
        for (int j = nl; j < 22; j++) p = pac(out, p, mx, ' ');
        p = pa(out, p, mx, packages[i].version);
        p = pa(out, p, mx, "  [");
        p = pa(out, p, mx, packages[i].category);
        p = pa(out, p, mx, "]  ");
        p = pai(out, p, mx, packages[i].size);
        p = pa(out, p, mx, " KB\n");
    }
    return p;
}

int PackageManager::cmd_info(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: pkginfo <package>\n");

    Package* pkg = Find(argv[1]);
    if (!pkg) return pa(out, 0, mx, "Package not found\n");

    int p = 0;
    p = pa(out, p, mx, "╔══════════════════════════════════╗\n");
    p = pa(out, p, mx, "║          Package Info            ║\n");
    p = pa(out, p, mx, "╠══════════════════════════════════╣\n");
    p = pa(out, p, mx, "║ Name:     "); p = pa(out, p, mx, pkg->name);
    for (int i = plen(pkg->name); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Version:  "); p = pa(out, p, mx, pkg->version);
    for (int i = plen(pkg->version); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Category: "); p = pa(out, p, mx, pkg->category);
    for (int i = plen(pkg->category); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Status:   ");
    p = pa(out, p, mx, pkg->state == PKG_INSTALLED ? "Installed            " : "Available            ");
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Size:     ");
    char sz[16]; int si = pai(sz, 0, 16, pkg->size);
    p = pa(out, p, mx, sz);
    p = pa(out, p, mx, " KB");
    for (int i = si + 3; i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "╠══════════════════════════════════╣\n");
    p = pa(out, p, mx, "║ "); p = pa(out, p, mx, pkg->description);
    int dl = plen(pkg->description);
    for (int i = dl; i < 31; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "╚══════════════════════════════════╝\n");
    return p;
}

Package* PackageManager::GetPackage(int idx) {
    if (idx < 0 || idx >= package_count) return nullptr;
    return &packages[idx];
}

int PackageManager::InstalledCount() {
    int n = 0;
    for (int i = 0; i < package_count; i++)
        if (packages[i].state == PKG_INSTALLED) n++;
    return n;
}

int PackageManager::AvailableCount() {
    int n = 0;
    for (int i = 0; i < package_count; i++)
        if (packages[i].state != PKG_INSTALLED) n++;
    return n;
}
