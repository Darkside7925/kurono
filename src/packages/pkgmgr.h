#pragma once
//  kurono os  -  package manager (kpkg)

#define PKG_MAX_PACKAGES  64
#define PKG_MAX_NAME      32
#define PKG_MAX_DESC      64
#define PKG_MAX_DEPS       8

enum PkgState {
    PKG_AVAILABLE = 0,
    PKG_INSTALLED = 1,
    PKG_UPDATING  = 2,
    PKG_BROKEN    = 3
};

struct Package {
    char name[PKG_MAX_NAME];
    char version[16];
    char description[PKG_MAX_DESC];
    char category[16];
    PkgState state;
    unsigned int size;        // kb
    char deps[PKG_MAX_DEPS][PKG_MAX_NAME];
    int  dep_count;
};

class PackageManager {
public:
    static void Init();

    static bool Install(const char* name);
    static bool Remove(const char* name);
    static bool Update(const char* name);
    static bool UpdateAll();

    static Package* Find(const char* name);
    static int  Search(const char* pattern, Package** results, int max_results);
    static int  ListInstalled(Package** results, int max_results);
    static int  ListAll(Package** results, int max_results);

    static Package* GetPackages();
    static int GetPackageCount();
    static Package* GetPackage(int idx);
    static int InstalledCount();
    static int AvailableCount();

    // shell integration
    static int cmd_install(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_remove(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_update(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_search(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_list(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_info(void* sh, int argc, const char** argv, char* out, int mx);

    static void RegisterCommands(void* shell);

private:
    static Package packages[PKG_MAX_PACKAGES];
    static int package_count;
    static void AddDefaultPackages();
};
