// Kurono OS Enhanced Kernel - Full Desktop Edition
// Complete bare-metal OS with hybrid kernel, desktop environment and apps

#include "types.h"
#include "multiboot.h"
#include "system.h"
#include "heap.h"
#include "time.h"
#include "memory_mgr.h"
#include "../drivers/serial.h"
#include "../drivers/display.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../media/mediadecoder.h"
#include "../ui/gui.h"
#include "../ui/lockscreen.h"
#include "../ui/font.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../apps/calculator.h"
#include "../apps/terminal.h"
#include "../apps/file_manager.h"
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../apps/task_manager.h"
#include "../hal/hal.h"
#include "../fs/vfs.h"
#include "../fs/kvfs.h"
#include "../proc/scheduler.h"
#include "../tests/test_suite.h"
#include "../system/input_manager.h"
#include "../shell/shell.h"
#include "../ui/wallpaper.h"
#include "../shell/linux_cmds.h"
#include "../shell/windows_cmds.h"
#include "../kcl/kcl.h"
#include "../security/supr.h"
#include "../packages/pkgmgr.h"
#include "../net/network.h"
#include "../drivers/audio.h"
#include "../drivers/e1000.h"
#include "../linux/dual_boot.h"
#include "../linux/linux_init.h"
#include "../linux/linux_netbridge.h"
#include "../linux/linux_drivers.h"
#include "../virt/vmm.h"
#include "../virt/vdevices.h"
#include "../virt/iommu.h"
#include "../virt/hypervisor.h"
#include "../drivers/nvidia_gpu.h"
#include "../../logo.h"

// Helper: string comparison (no libc)
static bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

extern "C" void kernel_main(uint64_t magic, uint64_t mb_addr) {
    SerialLogger::Init();
    SerialLogger::Log("Kurono OS Starting...\r\n");

    // Check multiboot magic FIRST
    if (magic != 0x2BADB002) {
        SerialLogger::Log("FATAL: Invalid Multiboot Magic!\r\n");
        while (true) { __asm__ __volatile__("cli; hlt"); }
    }
    SerialLogger::Log("Multiboot OK\r\n");

    // Initialize core subsystems
    SerialLogger::Log("[1] HAL::Init\r\n");
    HAL::Init();
    SerialLogger::Log("[2] MemoryManager::Init\r\n");
    MemoryManager::Init();
    SerialLogger::Log("[3] Scheduler::Init\r\n");
    Scheduler::Init();
    SerialLogger::Log("[4] VFS::Init\r\n");
    VFS::Init();
    SerialLogger::Log("[5] TestSuite::Run\r\n");
    // Run Diagnostics (after core init, before display)
    TestSuite::Run();
    SerialLogger::Log("[6] Post-TestSuite\r\n");

    multiboot_info_t* mbi = (multiboot_info_t*)mb_addr;

    // === Timer & Time (init before display, needed for WaitMs) ===
    Timer::Init(1000);
    TimeManager::SelectPIT(1000);
    TimeManager::Init();

    // NOTE: Interrupts stay DISABLED. The kernel is fully polling-based
    // (PIT counter read, keyboard/mouse I/O ports). This avoids WHPX
    // compatibility issues with hardware interrupt delivery.

    // === Display Initialization - BGA first, then multiboot framebuffer ===
    SerialLogger::Log("Initializing display...\r\n");

    bool has_display = false;

    // 1. Try Bochs Graphics Adapter (QEMU -vga std)
    if (BGA::Init(1024, 768, 32)) {
        Graphics::Init(BGA_FRAMEBUFFER_ADDR, BGA::width, BGA::height, BGA::pitch, (uint8_t)BGA::bpp);
        has_display = true;
        SerialLogger::Log("Display: BGA 1024x768x32 OK\r\n");
    }

    // 2. Fallback: multiboot-provided VBE framebuffer (GRUB)
    if (!has_display && (mbi->flags & (1u << 12))) {
        Graphics::Init(mbi->framebuffer_addr, mbi->framebuffer_width,
                      mbi->framebuffer_height, mbi->framebuffer_pitch, mbi->framebuffer_bpp);
        has_display = true;
        SerialLogger::Log("Display: Multiboot framebuffer OK\r\n");
    }

    if (!has_display) {
        SerialLogger::Log("Display: No framebuffer! Running headless.\r\n");
    }

    // Enable double buffering if we have a display
    if (has_display) {
        Graphics::SetTargetFPS(180);
        Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);

        // Verify display works — write directly to framebuffer
        SerialLogger::Log("Display: RenderMode=");
        SerialLogger::LogDec((int)Graphics::GetRenderMode());
        SerialLogger::Log(" backbuf=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::GetBackBuffer());
        SerialLogger::Log(" activebuf=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::GetBuffer());
        SerialLogger::Log("\r\n");
    }

    // === Enhanced Input Devices ===
    Keyboard::Init();
    Keyboard::InitUSB();

    Mouse::Init();
    Mouse::SetPollingRate(1000);
    Mouse::SetHighPrecision(true);
    Mouse::SetDPIScaling(1600, 1600);

    // === Boot Splash with embedded logo (logo.h) ===
    if (has_display) {
        int sw = Graphics::GetWidth();
        int sh = Graphics::GetHeight();

        // Pitch black screen
        Graphics::Clear(0xFF000000);

        // Draw embedded logo centered (scaled to 200x200)
        int logo_sw = 200, logo_sh = 200;
        int logo_lx = (sw - logo_sw) / 2;
        int logo_ly = (sh / 3) - (logo_sh / 2);

        for (int dy = 0; dy < logo_sh; dy++) {
            int src_y = (dy * LOGO_HEIGHT) / logo_sh;
            for (int dx = 0; dx < logo_sw; dx++) {
                int src_x = (dx * LOGO_WIDTH) / logo_sw;
                uint32_t pixel = logo_data[src_y * LOGO_WIDTH + src_x];
                uint8_t alpha = (pixel >> 24) & 0xFF;
                if (alpha > 64) {
                    Graphics::DrawPixel(logo_lx + dx, logo_ly + dy, pixel | 0xFF000000);
                }
            }
        }

        // "KURONO" text centered below logo
        const char* brand = "K U R O N O";
        int brand_w = 11 * 8; // approximate width
        Graphics::DrawString((sw - brand_w) / 2, logo_ly + logo_sh + 24, brand, 0xFFAAAAAA, 0xFF000000);

        // Loading bar dimensions
        int bar_w = 180, bar_h = 3;
        int bar_x = (sw - bar_w) / 2;
        int bar_y = logo_ly + logo_sh + 52;

        // Bar track (dark gray)
        Graphics::FillRect(bar_x, bar_y, bar_w, bar_h, 0xFF222222);
        Graphics::SwapBuffers();
        SerialLogger::Log("Boot splash: logo displayed\r\n");

        // Animate loading bar over ~3 seconds (60 steps x 50ms)
        for (int step = 1; step <= 60; step++) {
            int fill_w = (step * bar_w) / 60;
            // Smooth gradient fill: blue to cyan
            for (int px = 0; px < fill_w; px++) {
                int r = 0x30 + (px * 0x30) / bar_w;
                int g = 0x80 + (px * 0x60) / bar_w;
                int b = 0xFF;
                uint32_t c = 0xFF000000 | (r << 16) | (g << 8) | b;
                for (int py = 0; py < bar_h; py++)
                    Graphics::DrawPixel(bar_x + px, bar_y + py, c);
            }
            // Three pulsing dots after the bar
            int dot_y = bar_y + bar_h + 16;
            for (int d = 0; d < 3; d++) {
                int dot_x = (sw / 2) - 16 + d * 16;
                int bright = 80 + ((step + d * 8) % 20) * 8;
                if (bright > 255) bright = 255;
                uint32_t dc = 0xFF000000 | (bright << 16) | (bright << 8) | bright;
                Graphics::FillRect(dot_x, dot_y, 4, 4, dc);
            }
            Graphics::SwapBuffers();
            Timer::WaitMs(50);
        }

        // Brief pause then smooth clear
        Timer::WaitMs(200);
        Graphics::Clear(0xFF000000);
        Graphics::SwapBuffers();
        Timer::WaitMs(150);
        SerialLogger::Log("Boot splash complete\r\n");
    }

    // === System & Wallpaper ===
    System::Initialize();

    MediaDecoder::Image wallpaper = {0, 0, 0, false, 0, false};
    if (mbi->flags & (1u << 3) && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;

        // Try named wallpaper
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char* name = (const char*)mods[i].string;
            if (name && streq(name, "wallpaper")) {
                MediaDecoder::Image candidate = MediaDecoder::DecodeModule(mods[i].mod_start, mods[i].mod_end);
                if (candidate.valid) { wallpaper = candidate; break; }
            }
        }

        // Load font module
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char* name = (const char*)mods[i].string;
            SerialLogger::Log("Module: ");
            if (name) SerialLogger::Log(name); else SerialLogger::Log("(null)");
            SerialLogger::Log("\r\n");

            if (name && streq(name, "font")) {
                const uint8_t* fptr = (const uint8_t*)mods[i].mod_start;
                int fsize = (int)(mods[i].mod_end - mods[i].mod_start);
                FontTTF::Init(fptr, fsize);
                if (FontTTF::ok) SerialLogger::Log("Font OK\r\n");
                else SerialLogger::Log("Font FAIL\r\n");
                break;
            }
        }

        // Fallback wallpaper: try any remaining image
        if (!wallpaper.valid) {
            for (uint32_t i = 0; i < mbi->mods_count; i++) {
                uint32_t start = mods[i].mod_start;
                uint32_t end = mods[i].mod_end;
                const uint8_t* d = (const uint8_t*)start;
                size_t n = (size_t)(end - start);
                MediaDecoder::Image candidate;
                if (MediaDecoder::IsPNG(d, n) || MediaDecoder::IsJPEG(d, n)) {
                    candidate = MediaDecoder::DecodeModule(start, end);
                } else {
                    candidate = MediaDecoder::DecodeRaw(start);
                }
                if (candidate.valid) { wallpaper = candidate; break; }
            }
        }
    }

    // Fallback: use embedded wallpaper if no module wallpaper loaded
    if (!wallpaper.valid) {
        SerialLogger::Log("Loading embedded wallpaper...\r\n");
        uint32_t wp_start = (uint32_t)(uintptr_t)wallpaper_png_data;
        uint32_t wp_end = wp_start + wallpaper_png_size;
        wallpaper = MediaDecoder::DecodeModule(wp_start, wp_end);
        if (wallpaper.valid) {
            SerialLogger::Log("Embedded wallpaper decoded OK\r\n");
        } else {
            SerialLogger::Log("Embedded wallpaper decode FAILED\r\n");
        }
    }

    GUI::SetWallpaper(wallpaper);
    if (wallpaper.valid) {
        Desktop::SetWallpaperImage(wallpaper);
        SerialLogger::Log("Desktop wallpaper image set\r\n");
    }

    TimeManager::SetTimezoneMinutes(0);
    TimeManager::EnableDST(false);

    // === Initialize Kurono OS Subsystems ===
    SerialLogger::Log("[KVFS] Init...\r\n");
    KVFS::Init();

    SerialLogger::Log("[Shell] Init...\r\n");
    KuronoShell::Init();
    KuronoShell shell_instance;   // trivial object — all methods are static

    SerialLogger::Log("[LinuxCmds] Register...\r\n");
    LinuxCmds::RegisterAll(&shell_instance);

    SerialLogger::Log("[WindowsCmds] Register...\r\n");
    WindowsCmds::RegisterAll(&shell_instance);

    SerialLogger::Log("[SUPR] Init...\r\n");
    SUPR::Init();

    SerialLogger::Log("[PackageManager] Init...\r\n");
    PackageManager::Init();
    PackageManager::RegisterCommands(&shell_instance);

    SerialLogger::Log("[Network] Init...\r\n");
    Network::Init();
    WiFi::Init();

    SerialLogger::Log("[KCL] Init...\r\n");
    KCL::Init(&shell_instance);

    // === Linux Subsystem (Dual-Boot / Integrated Mode) ===
    SerialLogger::Log("[DualBoot] Init...\r\n");
    DualBootManager::Init();

    SerialLogger::Log("[DualBoot] Starting integrated boot...\r\n");
    DualBootManager::BootIntegrated();

    SerialLogger::Log("[LinuxNet] Init...\r\n");
    LinuxNetBridge::Init();

    // Register Linux subsystem shell commands
    LinuxInit::RegisterShellCommands(&shell_instance);
    DualBootManager::RegisterShellCommands(&shell_instance);
    LinuxNetBridge::RegisterShellCommands(&shell_instance);

    // === Linux Driver Framework ===
    SerialLogger::Log("[LinuxDrivers] Init...\r\n");
    LinuxDriverFramework::Init();
    LinuxDriverFramework::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Linux] Subsystem fully integrated\r\n");

    // === Populate KVFS filesystem ===
    KVFS::Mkdirs("/home/user/Documents");
    KVFS::Mkdirs("/home/user/Downloads");
    KVFS::Mkdirs("/home/user/Desktop");
    KVFS::Mkdirs("/home/user/Music");
    KVFS::Mkdirs("/usr/bin");
    KVFS::Mkdirs("/etc");
    KVFS::Mkdirs("/tmp");
    KVFS::Mkdirs("/var/log");
    KVFS::WriteString("/etc/hostname", "kurono");
    KVFS::WriteString("/etc/os-release", "Kurono OS v1.0\nARCH=x86\nKERNEL=kurono\n");
    KVFS::WriteString("/home/user/readme.txt", "Welcome to Kurono OS!\n\nThis is a bare-metal operating system.\nType 'help' in the terminal for available commands.\n");
    KVFS::WriteString("/home/user/Documents/denji.mp4", "[MP4 container - Denji - Chainsaw Man AMV - 1920x1080 H.264 AAC 3:42]");
    KVFS::WriteString("/home/user/Music/startup.wav", "[WAV PCM 22050Hz 16-bit stereo 0:05]");
    KVFS::WriteString("/home/user/Music/notification.wav", "[WAV PCM 22050Hz 16-bit mono 0:02]");
    KVFS::WriteString("/home/user/hello.kcl", "# KCL Script\nprint \"Hello from Kurono!\"\nset x 42\nprint x\n");
    KVFS::WriteString("/home/user/math.kcl", "# Math demo\nset a 16\nset b sqrt(a)\nprint \"sqrt(16) = \"\nprint b\nset r rand()\nprint \"random = \"\nprint r\n");
    KVFS::WriteString("/home/user/loop.kcl", "# Loop demo\nset sum 0\nfor i in 1 10 do\n  set sum sum + i\nend\nprint \"Sum 1..10 = \"\nprint sum\n");
    KVFS::WriteString("/home/user/fib.kcl", "# Fibonacci\nset a 0\nset b 1\nfor i in 1 10 do\n  set c a + b\n  print c\n  set a b\n  set b c\nend\n");
    SerialLogger::Log("[KVFS] Filesystem populated\r\n");

    // === Desktop Environment ===
    if (has_display) {
        SerialLogger::Log("[Desktop] Init...\r\n");
        DesktopEnvironment::Init(Graphics::GetWidth(), Graphics::GetHeight());

        LockScreen::Show();
    }

    // Calculator is now launched via Start Menu → WM, no standalone init

    // === Main Render Loop ===
    const uint32_t TARGET_FPS = 144;
    const uint32_t TARGET_FRAME_MS = 1000 / TARGET_FPS;  // ~6.9ms per frame

    uint32_t frame_counter = 0;
    uint32_t fps_counter = 0;
    uint32_t last_fps_ms = Timer::GetRealMs();
    uint32_t displayed_fps = 0;
    uint32_t frame_start_ms = Timer::GetRealMs();

    // Initialize audio driver (SB16)
    Audio::Init();

    // Initialize virtualization subsystem
    SerialLogger::Log("[VMM] Detecting hardware virtualization...\r\n");
    VMM::Init();
    if (VMM::IsSupported()) {
        SerialLogger::Log("[VMM] Virtualization: ");
        SerialLogger::Log(VMM::GetVendor());
        SerialLogger::Log(" (");
        SerialLogger::Log(VMM::GetType() == 1 ? "VT-x" : "AMD-V");
        SerialLogger::Log(") detected\r\n");
    } else {
        SerialLogger::Log("[VMM] No hardware virtualization available\r\n");
    }
    VirtualDevices::Init();

    // Initialize Hypervisor subsystem
    SerialLogger::Log("[Hypervisor] Init...\r\n");
    Hypervisor::Init();

    // Initialize NVIDIA GPU driver
    SerialLogger::Log("[GPU] Detecting NVIDIA GPU...\r\n");
    NvidiaGPU::Init();
    if (NvidiaGPU::IsDetected()) {
        const NvidiaGPUInfo& gi = NvidiaGPU::GetInfo();
        SerialLogger::Log("[GPU] ");
        SerialLogger::Log(gi.name);
        SerialLogger::Log(" detected\r\n");
    }

    // Initialize IOMMU (VT-d / AMD-Vi) for device passthrough
    SerialLogger::Log("[IOMMU] Detecting IOMMU...\r\n");
    IOMMU::Init();
    if (IOMMU::IsSupported() && NvidiaGPU::IsDetected()) {
        SerialLogger::Log("[IOMMU] VT-d available — GPU passthrough possible\r\n");
    }

    SerialLogger::Log("Entering main loop\r\n");

    Mouse::SetAutoDraw(false);

    while (true) {
        frame_start_ms = Timer::GetRealMs();

        // Advance system time by real elapsed ms (PIT-polled)
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) {
            TimeManager::AdvanceByMs(real_elapsed);
        }

        // Check for deferred resolution change (between frames, before rendering)
        if (frame_counter % 8 == 0) {
            SettingsApp::PollDeferredActions();
        }

        // Tick audio driver (poll-based buffer management)
        Audio::Tick();

        // Poll E1000 NIC — every 4th frame to reduce overhead
        if (E1000::IsDetected() && (frame_counter & 3) == 0) {
            E1000::Poll();
        }

        // Poll input
        InputManager::Poll();
        Mouse::Poll();
        Scheduler::Tick();

        if (has_display) {
            // Drain all mouse events from the ring buffer
            int scroll_delta = 0;
            while (Mouse::HasEvent()) {
                Mouse::Event mevt = Mouse::GetEvent();
                if (mevt.type == 3) scroll_delta += mevt.dz; // scroll
            }

            // Forward input to desktop environment
            bool mouse_clicked = Mouse::LeftClicked();
            bool mouse_down = Mouse::IsLeftDown(); // true while button held — enables dragging
            char kb_char = 0;
            if (Keyboard::HasChar()) { kb_char = Keyboard::GetChar(); }

            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, mouse_down, mouse_clicked, kb_char);

            // Forward scroll to focused window
            if (scroll_delta != 0) {
                Window* fw = WindowManager::GetFocusedWindow();
                if (fw && fw->input) {
                    fw->input(fw, 3, scroll_delta, 0); // event 3 = scroll
                }
            }

            DesktopEnvironment::Update();

            // Refresh task manager periodically — every 300 frames (~2s)
            if (frame_counter % 300 == 0) {
                TaskManagerApp::RefreshProcesses();
            }

            // Redraw every frame for full smoothness
            DesktopEnvironment::Render();

            // FPS counter update — using REAL PIT-polled time
            frame_counter++;
            fps_counter++;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_fps_ms >= 1000) {
                displayed_fps = fps_counter;
                fps_counter = 0;
                last_fps_ms = now_ms;
            }

            // Draw FPS overlay — sleek rounded pill
            {
                char fps_str[16] = "FPS ";
                char num_buf[8];
                int val = (int)displayed_fps;
                if (val == 0) { num_buf[0] = '0'; num_buf[1] = 0; }
                else {
                    char tmp[8]; int n = 0;
                    while (val > 0 && n < 7) { tmp[n++] = '0' + (val % 10); val /= 10; }
                    for (int i = 0; i < n; i++) num_buf[i] = tmp[n - 1 - i];
                    num_buf[n] = 0;
                }
                int si = 4;
                for (int i = 0; num_buf[i] && si < 14; i++) fps_str[si++] = num_buf[i];
                fps_str[si] = 0;
                int sw = Graphics::GetWidth();
                int pill_w = si * 8 + 16;
                int tx = sw - pill_w - 6;
                // Semi-transparent dark pill with accent border
                Graphics::FillRoundedRect(tx, 6, pill_w, 22, 11, 0xB0101020);
                Graphics::DrawString(tx + 8, 10, fps_str, 0xFF00E676, 0xFF000000);
            }

            Mouse::DrawAt(Mouse::mx, Mouse::my);
            Graphics::SwapBuffers();
        }

        // Frame pacing — cap to target FPS using hlt for CPU efficiency
        uint32_t frame_end_ms = Timer::GetRealMs();
        uint32_t frame_time = frame_end_ms - frame_start_ms;
        if (frame_time < TARGET_FRAME_MS) {
            uint32_t wait_until = frame_start_ms + TARGET_FRAME_MS;
            // pause reduces CPU power in spin-wait (works under WHPX/TCG)
            while (Timer::GetRealMs() < wait_until) {
                __asm__ __volatile__("pause");
            }
        }
    }
}
