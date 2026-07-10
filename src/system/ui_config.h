#pragma once
//  kurono os - ui configuration (backed by /etc/kurono/ui.conf)
//
//  this module provides a single source of truth for user-tweakable ui
//  colors, sizes, and toggles. it is read from a plain key=value file in
//  kvfs at /etc/kurono/ui.conf and can be reloaded at runtime via the
//  `kurono reload` shell command.
//
//  colors are 0xaarrggbb hex literals (no quotes).
//  integers are plain decimal.
//  booleans are 0 or 1.
//
//  if the config file does not exist, a default copy is written on first
//  boot. if a key is missing, the built-in default is returned.
//
//  any code that wants to react to reloads should call the version()
//  counter - every successful reload() bumps it.
//
#include "../kernel/types.h"

class UIConfig {
public:
    static void Init();         // create default file if missing, then load()
    static void Load();         // read file, parse, populate cache
    static bool Reload();       // re-read file at runtime, bump version()
    static uint32_t Version();  // bumped on every successful reload

    static uint32_t Color(const char* key, uint32_t fallback);
    static int      Int  (const char* key, int fallback);
    static bool     Bool (const char* key, bool fallback);
    static const char* Str(const char* key, const char* fallback);

    static const char* Path();  // "/etc/kurono/ui.conf"

    static const char* DefaultFile();

    // live update: write a single key, bump version, optionally persist
    // value is interpreted as a string ("0xFF112233" for colors, decimal
    // for ints, "0"/"1" for bools). callers are responsible for triggering
    // any subsystem ReloadFromConfig() they care about.
    static void Set(const char* key, const char* value, bool persist = true);
    static void SetColor(const char* key, uint32_t argb, bool persist = true);
    static void SetInt  (const char* key, int v, bool persist = true);

    // explicitly write current entries back to /etc/kurono/ui.conf.
    static bool Save();

private:
    static const int MAX_ENTRIES = 128;
    static const int MAX_KEY_LEN = 48;
    static const int MAX_VAL_LEN = 48;

    struct Entry {
        char key[MAX_KEY_LEN];
        char val[MAX_VAL_LEN];
        bool used;
    };

    static Entry entries[MAX_ENTRIES];
    static int   entry_count;
    static uint32_t version_counter;
    static bool  initialized;

    static int  Find(const char* key);
    static void Put (const char* key, const char* val);
    static void Clear();
    static void ParseLine(const char* line);
    static uint32_t ParseHex(const char* s, uint32_t fallback);
    static int      ParseInt(const char* s, int fallback);
};
