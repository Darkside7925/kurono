#pragma once
#include "ui_elements.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"

// simple virtual file system for demo
struct VirtualFile {
    char name[64];
    bool is_dir;
    uint32_t size;
    VirtualFile* parent;
    VirtualFile* children[32];
    int child_count;
    
    VirtualFile(const char* n, bool dir, uint32_t s) {
        int i=0; while(n[i] && i<63) { name[i]=n[i]; i++; } name[i]=0;
        is_dir = dir;
        size = s;
        parent = nullptr;
        child_count = 0;
        for(int k=0; k<32; k++) children[k] = nullptr;
    }
    
    void Add(VirtualFile* f) {
        if (child_count < 32) {
            children[child_count++] = f;
            f->parent = this;
        }
    }
};

class FileBrowser {
public:
    static bool visible;
    static int x, y, w, h;
    static char selected_file[256];
    
    static void Init();
    static void Show();
    static void Hide();
    static void Draw();
    static void OnClick(int mx, int my);
    
private:
    static VirtualFile* root;
    static VirtualFile* current_dir;
    static VirtualFile* selected_node;
    
    static void DrawWindowFrame();
    static void DrawSidebar();
    static void DrawContent();
    static void DrawIcon(int x, int y, bool is_dir, const char* name);
    static void DrawBreadcrumbs();
    
    static void NavigateUp();
    static void Navigate(VirtualFile* dir);
};
