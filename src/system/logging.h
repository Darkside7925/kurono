#pragma once

//  kurono runtime logging - lightweight, no daemon, just clean structured files
//  under /kurono/var/log (see kpaths.h). minimal by design: one file per
//  category plus per-app / per-process sublogs. (satoru)

namespace RuntimeLog {
    void InitFilesystem();
    void MirrorSerial(const char* text);
    // move the staged serial-mirror text into kvfs. call from PROCESS context
    // only (the LoggingProcess) - MirrorSerial itself never touches kvfs. (satoru)
    void FlushSerialMirror();
    void LogSystem(const char* component, const char* message);
    void LogBoot(const char* message);
    // network events - connect / disconnect / link state / errors. (satoru)
    void LogNetwork(const char* event, const char* detail = nullptr);
    // security events - supr escalations, ksa prompts, grants/denials. (satoru)
    void LogSecurity(const char* event, const char* detail = nullptr);
    // crash / panic record - appended to /kurono/var/log/crash/<n>.log. (satoru)
    void LogCrash(const char* summary, const char* detail = nullptr);
    void LogAppEvent(const char* app, const char* event, const char* detail = nullptr);
    void LogProcessEvent(const char* process_name, int pid, const char* event, const char* detail = nullptr);
}
