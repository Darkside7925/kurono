//  kurono os  -  ui configuration implementation
//
//  parser is deliberately small: one pass, line oriented, `key = value`,
//  `#` comments, blank lines allowed. values are stored as strings and
//  parsed on demand by the typed accessors.
//
//  the file lives in kvfs so it survives reboots only inside the live
//  image, which matches the way the rest of the project treats config
//  data. the default file written on first boot documents every key.
//
#include "ui_config.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"

UIConfig::Entry UIConfig::entries[UIConfig::MAX_ENTRIES];
int             UIConfig::entry_count     = 0;
uint32_t        UIConfig::version_counter = 0;
bool            UIConfig::initialized     = false;

static int uic_slen(const char* s){ int n=0; if(s) while(s[n]) n++; return n; }

static bool uic_seq(const char* a, const char* b){
    if(!a||!b) return false;
    int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return false; i++; }
    return a[i]==b[i];
}

static void uic_scpy(char* d, const char* s, int mx){
    int i=0; if(s) while(s[i]&&i<mx-1){ d[i]=s[i]; i++; }
    d[i]=0;
}

static bool uic_is_ws(char c){ return c==' '||c=='\t'||c=='\r'; }

const char* UIConfig::Path(){ return "/etc/kurono/ui.conf"; }

const char* UIConfig::DefaultFile(){
    return
    "# ═══════════════════════════════════════════════════════════════════\n"
    "#  Kurono OS  -  UI configuration\n"
    "# ═══════════════════════════════════════════════════════════════════\n"
    "#\n"
    "#  Format: key = value\n"
    "#  Colors are 0xAARRGGBB hex. Sizes are integers. Bools are 0 or 1.\n"
    "#  Apply changes at runtime with: kurono reload\n"
    "#\n"
    "\n"
    "# ─── taskbar ──────────────────────────────────────────────────────────\n"
    "taskbar.height          = 44\n"
    "taskbar.position        = bottom           # bottom | top\n"
    "taskbar.bg              = 0xFF0C0C14\n"
    "taskbar.top_edge        = 0xFF2A2A40\n"
    "taskbar.text            = 0xFFBBBBCC\n"
    "taskbar.start_btn_bg    = 0xFF5C8AFF\n"
    "taskbar.start_btn_hover = 0xFF4470E0\n"
    "taskbar.show_clock      = 1\n"
    "taskbar.show_battery    = 1\n"
    "taskbar.show_wifi       = 1\n"
    "taskbar.show_volume     = 1\n"
    "taskbar.show_search     = 1\n"
    "\n"
    "# ─── desktop ──────────────────────────────────────────────────────────\n"
    "desktop.bg              = 0xFF0C0818\n"
    "desktop.icon_text       = 0xFFE8E8F0\n"
    "desktop.icon_selected   = 0xFF2A3860\n"
    "desktop.icon_size       = 56\n"
    "desktop.icon_spacing_x  = 96\n"
    "desktop.icon_spacing_y  = 100\n"
    "desktop.icon_margin_x   = 24\n"
    "desktop.icon_margin_y   = 20\n"
    "desktop.allow_edit      = 1               # 1 allows create/delete on desktop\n"
    "\n"
    "# ─── context menu ─────────────────────────────────────────────────────\n"
    "ctxmenu.bg              = 0xFF121228\n"
    "ctxmenu.border          = 0xFF5C8AFF\n"
    "ctxmenu.text            = 0xFFE8E8F0\n"
    "ctxmenu.item_h          = 30\n"
    "ctxmenu.width           = 180\n"
    "\n"
    "# ─── window manager ──────────────────────────────────────────────────\n"
    "window.titlebar_height  = 36\n"
    "window.corner_radius    = 10\n"
    "window.shadow_size      = 6\n"
    "window.title_bg         = 0xFF1C1C2E\n"
    "window.title_focused    = 0xFF22223A\n"
    "window.title_text       = 0xFFF0F0F5\n"
    "window.border_focus     = 0xFF6C8CFF\n"
    "window.close_btn        = 0xFFFF5F57\n"
    "window.min_btn          = 0xFFFFBD2E\n"
    "window.max_btn          = 0xFF28C840\n"
    "\n"
    "# ─── task manager ─────────────────────────────────────────────────────\n"
    "taskmgr.row_h           = 20\n"
    "taskmgr.allow_kill      = 1               # 1 enables Kill/Restart menu\n"
    "\n";
}

int UIConfig::Find(const char* key){
    for(int i=0;i<entry_count;i++){
        if(entries[i].used && uic_seq(entries[i].key, key)) return i;
    }
    return -1;
}

void UIConfig::Put(const char* key, const char* val){
    int idx = Find(key);
    if(idx < 0){
        if(entry_count >= MAX_ENTRIES) return;
        idx = entry_count++;
        entries[idx].used = true;
    }
    uic_scpy(entries[idx].key, key, MAX_KEY_LEN);
    uic_scpy(entries[idx].val, val, MAX_VAL_LEN);
}

void UIConfig::Clear(){
    for(int i=0;i<MAX_ENTRIES;i++){
        entries[i].used = false;
        entries[i].key[0] = 0;
        entries[i].val[0] = 0;
    }
    entry_count = 0;
}

void UIConfig::ParseLine(const char* line){
    if(!line) return;
    // skip leading whitespace
    int i = 0;
    while(line[i] && uic_is_ws(line[i])) i++;
    if(!line[i] || line[i]=='#' || line[i]=='\n') return;

    // extract key
    char key[MAX_KEY_LEN]; int ki = 0;
    while(line[i] && line[i]!='=' && !uic_is_ws(line[i]) && ki<MAX_KEY_LEN-1){
        key[ki++] = line[i++];
    }
    key[ki] = 0;
    if(ki == 0) return;

    // expect '=' (possibly after whitespace)
    while(line[i] && uic_is_ws(line[i])) i++;
    if(line[i] != '=') return;
    i++;
    while(line[i] && uic_is_ws(line[i])) i++;

    // extract value up to eol or '#' (comment)
    char val[MAX_VAL_LEN]; int vi = 0;
    while(line[i] && line[i]!='\n' && line[i]!='#' && vi<MAX_VAL_LEN-1){
        val[vi++] = line[i++];
    }
    // trim trailing whitespace
    while(vi > 0 && uic_is_ws(val[vi-1])) vi--;
    val[vi] = 0;
    if(vi == 0) return;

    Put(key, val);
}

uint32_t UIConfig::ParseHex(const char* s, uint32_t fallback){
    if(!s || !s[0]) return fallback;
    int i = 0;
    if(s[0]=='0' && (s[1]=='x'||s[1]=='X')) i = 2;
    uint32_t v = 0;
    bool any = false;
    while(s[i]){
        char c = s[i++];
        uint32_t d;
        if(c>='0' && c<='9') d = (uint32_t)(c-'0');
        else if(c>='a' && c<='f') d = (uint32_t)(c-'a'+10);
        else if(c>='A' && c<='F') d = (uint32_t)(c-'A'+10);
        else break;
        v = (v<<4) | d;
        any = true;
    }
    return any ? v : fallback;
}

int UIConfig::ParseInt(const char* s, int fallback){
    if(!s || !s[0]) return fallback;
    int i = 0; int sign = 1;
    if(s[0]=='-'){ sign=-1; i=1; }
    int v = 0; bool any = false;
    while(s[i]>='0' && s[i]<='9'){
        v = v*10 + (s[i]-'0');
        i++; any = true;
    }
    return any ? v*sign : fallback;
}

void UIConfig::Init(){
    if(initialized) return;
    initialized = true;

    // ensure parent directory exists
    KVFS::Mkdirs("/etc/kurono");

    // write default file on first boot if missing
    if(!KVFS::Exists(Path())){
        KVFS::WriteString(Path(), DefaultFile());
        SerialLogger::Log("[UIConfig] wrote default config to ");
        SerialLogger::Log(Path());
        SerialLogger::Log("\r\n");
    }

    Load();
}

void UIConfig::Load(){
    Clear();

    // read config file into buffer
    static char buf[4096];
    int n = KVFS::ReadString(Path(), buf, sizeof(buf));
    if(n <= 0){
        // no file  -  rely on fallbacks. still counts as a successful load.
        SerialLogger::Log("[UIConfig] no config file, using defaults\r\n");
        return;
    }

    // walk lines
    int start = 0;
    for(int i=0; i<=n; i++){
        if(i==n || buf[i]=='\n'){
            char save = buf[i];
            buf[i] = 0;
            ParseLine(buf + start);
            buf[i] = save;
            start = i + 1;
        }
    }

    SerialLogger::Log("[UIConfig] loaded ");
    SerialLogger::LogDec(entry_count);
    SerialLogger::Log(" entries\r\n");
}

bool UIConfig::Reload(){
    Load();
    version_counter++;
    return true;
}

uint32_t UIConfig::Version(){ return version_counter; }

uint32_t UIConfig::Color(const char* key, uint32_t fallback){
    int idx = Find(key);
    if(idx < 0) return fallback;
    return ParseHex(entries[idx].val, fallback);
}

int UIConfig::Int(const char* key, int fallback){
    int idx = Find(key);
    if(idx < 0) return fallback;
    return ParseInt(entries[idx].val, fallback);
}

bool UIConfig::Bool(const char* key, bool fallback){
    int idx = Find(key);
    if(idx < 0) return fallback;
    const char* v = entries[idx].val;
    if(v[0]=='1' || uic_seq(v,"true") || uic_seq(v,"yes") || uic_seq(v,"on")) return true;
    if(v[0]=='0' || uic_seq(v,"false")|| uic_seq(v,"no")  || uic_seq(v,"off")) return false;
    return fallback;
}

const char* UIConfig::Str(const char* key, const char* fallback){
    int idx = Find(key);
    if(idx < 0) return fallback;
    return entries[idx].val;
}
