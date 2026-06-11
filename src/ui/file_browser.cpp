#include "file_browser.h"
#include "../drivers/serial.h"

bool FileBrowser::visible = false;
int FileBrowser::x = 0;
int FileBrowser::y = 0;
int FileBrowser::w = 0;
int FileBrowser::h = 0;
char FileBrowser::selected_file[256] = {0};
VirtualFile* FileBrowser::root = nullptr;
VirtualFile* FileBrowser::current_dir = nullptr;
VirtualFile* FileBrowser::selected_node = nullptr;

void FileBrowser::Init() {
    if (root) return;
    
    // Create File Structure
    root = new VirtualFile("Root", true, 0);
    
    VirtualFile* c_drive = new VirtualFile("C:", true, 0);
    root->Add(c_drive);
    
    VirtualFile* users = new VirtualFile("Users", true, 0);
    c_drive->Add(users);
    
    VirtualFile* admin = new VirtualFile("Admin", true, 0);
    users->Add(admin);
    
    admin->Add(new VirtualFile("Documents", true, 0));
    admin->Add(new VirtualFile("Downloads", true, 0));
    VirtualFile* pics = new VirtualFile("Pictures", true, 0);
    admin->Add(pics);
    
    pics->Add(new VirtualFile("logo.png", false, 1024));
    pics->Add(new VirtualFile("wallpaper.png", false, 2048));
    pics->Add(new VirtualFile("user.jpg", false, 512));
    pics->Add(new VirtualFile("screenshot.bmp", false, 4096));
    
    VirtualFile* sys = new VirtualFile("System", true, 0);
    c_drive->Add(sys);
    sys->Add(new VirtualFile("kernel.elf", false, 12000));
    sys->Add(new VirtualFile("drivers.sys", false, 500));
    
    current_dir = admin; // Start in user folder
}

void FileBrowser::Show() {
    Init();
    visible = true;
    selected_file[0] = 0;
    // Center Window
    w = 800; h = 600;
    x = (Graphics::GetWidth() - w) / 2;
    y = (Graphics::GetHeight() - h) / 2;
}

void FileBrowser::Hide() { visible = false; }

void FileBrowser::Draw() {
    if (!visible) return;
    
    DrawWindowFrame();
    DrawSidebar();
    DrawContent();
}

void FileBrowser::DrawWindowFrame() {
    // Main Background
    Graphics::FillRectRounded(x, y, w, h, 12, 0xFF252526);
    
    // Title Bar
    Graphics::FillRectRounded(x, y, w, 40, 12, 0xFF333333);
    // Fix rounded corners at bottom of title bar being square
    Graphics::FillRect(x, y + 20, w, 20, 0xFF333333); 
    
    // Title
    FontTTF::DrawString(x + 20, y + 10, 18.0f, "File Explorer", 0xFFFFFFFF);
    
    // Window Controls
    int cx = x + w - 20;
    int cy = y + 10;
    // Close (Red)
    Graphics::FillRectRounded(cx - 20, cy, 20, 20, 10, 0xFFFF5555);
    // Maximize (Yellow)
    Graphics::FillRectRounded(cx - 50, cy, 20, 20, 10, 0xFFFFBD44);
    // Minimize (Green)
    Graphics::FillRectRounded(cx - 80, cy, 20, 20, 10, 0xFF00CA4E);
}

void FileBrowser::DrawSidebar() {
    int sb_w = 200;
    int sb_y = y + 40;
    int sb_h = h - 40;
    
    // Sidebar BG
    Graphics::FillRect(x, sb_y, sb_w, sb_h, 0xFF1E1E1E);
    // Divider
    Graphics::FillRect(x + sb_w, sb_y, 1, sb_h, 0xFF444444);
    
    // Quick Access Items
    const char* items[] = { "Desktop", "Documents", "Downloads", "Pictures", "Music", "This PC" };
    int iy = sb_y + 20;
    
    for (int i=0; i<6; i++) {
        // Highlight "This PC" or "Documents" logic? Just fake it for now
        bool active = (i == 1); 
        if (active) Graphics::FillRectRounded(x + 10, iy - 5, sb_w - 20, 30, 6, 0xFF37373D);
        
        // Icon placeholder (simple dot)
        Graphics::FillRect(x + 25, iy + 2, 16, 16, active ? 0xFF007ACC : 0xFF888888);
        
        FontTTF::DrawString(x + 50, iy, 16.0f, items[i], active ? 0xFFFFFFFF : 0xFFCCCCCC);
        iy += 40;
    }
}

void FileBrowser::DrawBreadcrumbs() {
    int bc_x = x + 220;
    int bc_y = y + 50;
    
    // Back Button
    Graphics::FillRectRounded(bc_x, bc_y, 30, 30, 4, 0xFF333333);
    FontTTF::DrawString(bc_x + 10, bc_y + 5, 20.0f, "<", 0xFFFFFFFF);
    
    // Up Button
    Graphics::FillRectRounded(bc_x + 40, bc_y, 30, 30, 4, 0xFF333333);
    FontTTF::DrawString(bc_x + 50, bc_y + 5, 20.0f, "^", 0xFFFFFFFF);
    
    // Path Bar
    Graphics::FillRectRounded(bc_x + 80, bc_y, w - 220 - 100, 30, 4, 0xFF2D2D30);
    
    char path[256]; path[0] = 0;
    (void)path;
    // Build path (reversed, then reverse back, simplified for now)
    VirtualFile* p = current_dir;
    (void)p;
    // Just show current name for simplicity in this demo
    FontTTF::DrawString(bc_x + 90, bc_y + 5, 16.0f, current_dir ? current_dir->name : "Root", 0xFFFFFFFF);
}

void FileBrowser::DrawContent() {
    DrawBreadcrumbs();
    
    int c_x = x + 220;
    int c_y = y + 90;
    int c_w = w - 220;
    (void)c_w;
    
    if (!current_dir) return;
    
    int grid_cols = 4;
    int grid_w = 100;
    int grid_h = 100;
    int gap = 20;
    
    for (int i=0; i < current_dir->child_count; i++) {
        VirtualFile* f = current_dir->children[i];
        if (!f) continue;
        
        int row = i / grid_cols;
        int col = i % grid_cols;
        
        int item_x = c_x + col * (grid_w + gap);
        int item_y = c_y + row * (grid_h + gap);
        
        bool selected = (selected_node == f);
        
        if (selected) {
            Graphics::FillRectRounded(item_x - 5, item_y - 5, grid_w + 10, grid_h + 10, 8, 0xFF264F78);
        }
        
        DrawIcon(item_x + 25, item_y + 10, f->is_dir, f->name);
        
        // Truncate name if too long
        FontTTF::DrawStringCenter(item_x + grid_w/2, item_y + 70, 14.0f, f->name, 0xFFFFFFFF);
    }
}

void FileBrowser::DrawIcon(int x, int y, bool is_dir, const char* name) {
    (void)name;
    if (is_dir) {
        // Folder Icon (Gold)
        // Back part
        Graphics::FillRectRounded(x + 5, y, 40, 35, 4, 0xFFD4A017); 
        // Tab
        Graphics::FillRectRounded(x + 5, y - 5, 20, 10, 4, 0xFFD4A017);
        // Front part
        Graphics::FillRectRounded(x, y + 5, 50, 30, 4, 0xFFFFD700);
    } else {
        // File Icon (White page)
        Graphics::FillRectRounded(x + 10, y, 30, 40, 2, 0xFFEEEEEE);
        // Fold corner
        Graphics::FillRect(x + 30, y, 10, 10, 0xFFCCCCCC);
        
        // Lines
        Graphics::FillRect(x + 15, y + 10, 20, 2, 0xFF888888);
        Graphics::FillRect(x + 15, y + 15, 20, 2, 0xFF888888);
        Graphics::FillRect(x + 15, y + 20, 20, 2, 0xFF888888);
    }
}

void FileBrowser::OnClick(int mx, int my) {
    if (!visible) return;
    
    // Close Button
    if (mx >= x + w - 40 && mx <= x + w - 10 && my >= y + 10 && my <= y + 30) {
        Hide();
        return;
    }
    
    // Navigation (Up)
    int bc_x = x + 220;
    int bc_y = y + 50;
    if (mx >= bc_x + 40 && mx <= bc_x + 70 && my >= bc_y && my <= bc_y + 30) {
        NavigateUp();
        return;
    }
    
    // Content Click
    int c_x = x + 220;
    int c_y = y + 90;
    
    if (mx >= c_x && my >= c_y && current_dir) {
        int grid_cols = 4;
        int grid_w = 100;
        int grid_h = 100;
        int gap = 20;
        
        // Rel pos
        int rx = mx - c_x;
        int ry = my - c_y;
        
        // Crude hit test
        int col = rx / (grid_w + gap);
        int row = ry / (grid_h + gap);
        
        // Check if inside item bounds roughly
        int cell_x = col * (grid_w + gap);
        int cell_y = row * (grid_h + gap);
        
        if (rx >= cell_x && rx <= cell_x + grid_w && ry >= cell_y && ry <= cell_y + grid_h) {
            int idx = row * grid_cols + col;
            if (idx >= 0 && idx < current_dir->child_count) {
                VirtualFile* target = current_dir->children[idx];
                
                if (selected_node == target) {
                    // Double click logic (simulated by clicking selected again)
                    if (target->is_dir) {
                        Navigate(target);
                    } else {
                        // Select file
                        int i=0; while(target->name[i]) { selected_file[i] = target->name[i]; i++; } selected_file[i] = 0;
                        Hide(); // "Open" file
                    }
                } else {
                    selected_node = target;
                }
            } else {
                selected_node = nullptr;
            }
        } else {
             selected_node = nullptr;
        }
    }
}

void FileBrowser::NavigateUp() {
    if (current_dir && current_dir->parent) {
        current_dir = current_dir->parent;
        selected_node = nullptr;
    }
}

void FileBrowser::Navigate(VirtualFile* dir) {
    if (dir && dir->is_dir) {
        current_dir = dir;
        selected_node = nullptr;
    }
}
