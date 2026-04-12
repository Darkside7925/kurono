#pragma once

namespace RuntimeLog {
    void InitFilesystem();
    void MirrorSerial(const char* text);
    void LogSystem(const char* component, const char* message);
    void LogBoot(const char* message);
    void LogAppEvent(const char* app, const char* event, const char* detail = nullptr);
}