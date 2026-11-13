//  kurono os - settings module: language & region (satoru)
//  the language (english for now), the region with its formats (clock, date
//  order, first day of the week) and the keyboard layout. everything is a
//  ui.conf key, so the kernel and the ring-3 build share this page: kbd.layout,
//  locale.region, clock.24h, locale.datefmt, locale.weekstart. (satoru)
#include "system_settings.h"
#include "settings_kit.h"
#include "../system/ui_config.h"
#include "../system/key_layouts.h"

using namespace SettingsUI;

namespace {

struct Region { const char* id; const char* name; bool h24; int datefmt; int weekstart; };
// datefmt: 0 day first, 1 month first, 2 year first. weekstart: 0 monday, 1 sunday. (satoru)
const Region kRegions[] = {
    { "us", "United States",  false, 1, 1 },
    { "gb", "United Kingdom", true,  0, 0 },
    { "de", "Germany",        true,  0, 0 },
    { "fr", "France",         true,  0, 0 },
    { "es", "Spain",          true,  0, 0 },
    { "it", "Italy",          true,  0, 0 },
    { "jp", "Japan",          true,  2, 1 },
    { "ca", "Canada",         false, 2, 1 },
    { "au", "Australia",      false, 0, 0 },
};
const int kRegionCount = (int)(sizeof(kRegions) / sizeof(kRegions[0]));

bool same(const char* a, const char* b){
    while(*a && *a == *b){ a++; b++; }
    return *a == *b;
}

int  s_region = 0, s_layout = 0, s_datefmt = 1, s_week = 1;
bool s_h24 = false;

void load(){
    const char* r = UIConfig::Str("locale.region", "us");
    s_region = 0;
    for(int i = 0; i < kRegionCount; i++) if(same(kRegions[i].id, r)) s_region = i;
    const char* l = UIConfig::Str("kbd.layout", "us");
    s_layout = 0;
    for(int i = 0; i < KeyLayouts::kLayoutCount; i++) if(same(KeyLayouts::kLayouts[i].id, l)) s_layout = i;
    s_h24 = UIConfig::Bool("clock.24h", kRegions[s_region].h24);
    s_datefmt = UIConfig::Int("locale.datefmt", kRegions[s_region].datefmt);
    s_week = UIConfig::Int("locale.weekstart", kRegions[s_region].weekstart);
}

void language_on_show(){ load(); }

void language_build(){
    Group("Language");
    Info("Language", "English", "Kurono's text is in English for now");
    EndGroup();

    Group("Region", "Formats follow the region until you change them.");
    const char* rnames[16];
    for(int i = 0; i < kRegionCount && i < 16; i++) rnames[i] = kRegions[i].name;
    if(Dropdown("Region", rnames, kRegionCount, &s_region)){
        const Region& rg = kRegions[s_region];
        UIConfig::Set("locale.region", rg.id, true);
        s_h24 = rg.h24; s_datefmt = rg.datefmt; s_week = rg.weekstart;
        UIConfig::SetInt("clock.24h", s_h24 ? 1 : 0, true);
        UIConfig::SetInt("locale.datefmt", s_datefmt, true);
        UIConfig::SetInt("locale.weekstart", s_week, true);
        UIConfig::Save();
    }
    if(Toggle("24-hour time", &s_h24)){ UIConfig::SetInt("clock.24h", s_h24 ? 1 : 0, true); UIConfig::Save(); }
    static const char* const kDate[3] = { "11 Nov", "Nov 11", "2026-11-11" };
    if(Segmented("Date", kDate, 3, &s_datefmt)){ UIConfig::SetInt("locale.datefmt", s_datefmt, true); UIConfig::Save(); }
    static const char* const kWeek[2] = { "Monday", "Sunday" };
    if(Segmented("Week starts on", kWeek, 2, &s_week)){ UIConfig::SetInt("locale.weekstart", s_week, true); UIConfig::Save(); }
    EndGroup();

    Group("Keyboard", "Apps opened after a change use the new layout.");
    const char* lnames[16];
    for(int i = 0; i < KeyLayouts::kLayoutCount && i < 16; i++) lnames[i] = KeyLayouts::kLayouts[i].name;
    if(Dropdown("Layout", lnames, KeyLayouts::kLayoutCount, &s_layout)){
        UIConfig::Set("kbd.layout", KeyLayouts::kLayouts[s_layout].id, true);
        UIConfig::Save();
    }
    EndGroup();
}

}  // namespace

extern const SettingsModule g_language_module = {
    "language", "Language & Region", "", language_on_show, nullptr, nullptr, nullptr, language_build
};
// end (satoru)
