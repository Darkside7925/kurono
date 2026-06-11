#pragma once

class System {
public:
    static void Initialize();
private:
    static void Wait(int count);
    static void LoadGUI();
    static void LoadGraphics();
    static void LoadVulkan();
    static void LoadOpenGL();
};
