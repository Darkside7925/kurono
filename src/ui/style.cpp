#include "style.h"
#include "font.h"
#include "../system/ui_config.h"
#include "../drivers/graphics.h"

namespace UIStyle {

static bool g_modern = true;
static bool g_minimal_icons = false;

static bool streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

bool Modern() { return g_modern; }

void Apply() {
    g_modern = !streq(UIConfig::Str("ui.style", "modern"), "classic");
    g_minimal_icons = streq(UIConfig::Str("ui.icons", "colorful"), "minimal");
    FontTTF::SetActive(g_modern);
    Graphics::MarkUIDirty();
}

void Set(bool modern) {
    UIConfig::Set("ui.style", modern ? "modern" : "classic");
    Apply();
}

bool MinimalIcons() { return g_minimal_icons; }

void SetMinimalIcons(bool minimal) {
    UIConfig::Set("ui.icons", minimal ? "minimal" : "colorful");
    Apply();
}

}
