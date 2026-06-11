"""
Kurono OS — Theme System
Glassmorphism dark theme with purple/blue accent colors.
"""

import tkinter as tk
import tkinter.ttk as ttk
from typing import Dict, Any


# ══════════════════════════════════════════════════════════════════════════════
#  Color Palette
# ══════════════════════════════════════════════════════════════════════════════

KURONO_THEME = {
    # Backgrounds
    "bg_deep":       "#07071A",
    "bg_base":       "#0D0D2B",
    "bg_surface":    "#12123A",
    "bg_elevated":   "#1A1A4A",
    "bg_card":       "#1E1E52",
    "bg_hover":      "#252560",
    "bg_active":     "#2C2C70",
    "bg_input":      "#0F0F30",
    "bg_glass":      "#15153580",   # with alpha-ish effect via stipple
    "bg_sidebar":    "#0A0A22",
    "bg_titlebar":   "#080820",

    # Accents
    "accent":        "#7C6FFF",
    "accent_bright": "#A89BFF",
    "accent_dim":    "#4A40CC",
    "accent2":       "#3BFFD8",     # cyan secondary
    "accent2_dim":   "#20B89A",
    "accent3":       "#FF6FBF",     # pink tertiary

    # Text
    "text_primary":  "#E8E8FF",
    "text_secondary":"#9999CC",
    "text_muted":    "#5A5A88",
    "text_disabled": "#3A3A55",
    "text_accent":   "#A89BFF",
    "text_success":  "#3BFFD8",
    "text_warning":  "#FFD700",
    "text_error":    "#FF4D6D",
    "text_info":     "#6EC8FF",

    # Borders
    "border":        "#2A2A6A",
    "border_focus":  "#7C6FFF",
    "border_bright": "#5050A0",

    # Specific
    "taskbar_bg":    "#080820",
    "taskbar_btn":   "#1A1A4A",
    "menu_bg":       "#10102E",
    "tooltip_bg":    "#1A1A4A",
    "shadow":        "#00000060",
    "selection":     "#7C6FFF40",

    # Terminal colors
    "term_bg":       "#060618",
    "term_fg":       "#E8E8FF",
    "term_cursor":   "#7C6FFF",
    "term_green":    "#3BFFD8",
    "term_yellow":   "#FFD700",
    "term_red":      "#FF4D6D",
    "term_blue":     "#6EC8FF",
    "term_magenta":  "#FF6FBF",
    "term_cyan":     "#3BFFD8",

    # Status bar
    "status_bg":     "#080820",
    "status_fg":     "#7C6FFF",
}

# Font definitions
FONTS = {
    "ui":        ("Segoe UI",     10, "normal"),
    "ui_bold":   ("Segoe UI",     10, "bold"),
    "ui_small":  ("Segoe UI",      9, "normal"),
    "ui_large":  ("Segoe UI",     12, "normal"),
    "ui_title":  ("Segoe UI",     13, "bold"),
    "ui_header": ("Segoe UI",     16, "bold"),
    "mono":      ("Consolas",     10, "normal"),
    "mono_bold": ("Consolas",     10, "bold"),
    "mono_large":("Consolas",     12, "normal"),
    "icon":      ("Segoe UI Emoji",14,"normal"),
    "clock":     ("Segoe UI",     11, "normal"),
    "logo":      ("Segoe UI",     18, "bold"),
    "desktop":   ("Segoe UI",      9, "normal"),
}

C = KURONO_THEME  # shortcut


# ══════════════════════════════════════════════════════════════════════════════
#  TTK Style Application
# ══════════════════════════════════════════════════════════════════════════════

def apply_theme(root: tk.Tk):
    """Apply the full Kurono glassmorphism theme to ttk widgets."""
    style = ttk.Style(root)
    style.theme_use("clam")

    # ── Frame / Labelframe ─────────────────────────────────────────────────
    style.configure("TFrame",
        background=C["bg_base"], borderwidth=0)
    style.configure("Card.TFrame",
        background=C["bg_card"], borderwidth=1, relief="flat")
    style.configure("Surface.TFrame",
        background=C["bg_surface"], borderwidth=0)
    style.configure("Sidebar.TFrame",
        background=C["bg_sidebar"], borderwidth=0)
    style.configure("TLabelframe",
        background=C["bg_card"], bordercolor=C["border"],
        borderwidth=1, relief="flat", labelmargins=8)
    style.configure("TLabelframe.Label",
        background=C["bg_card"], foreground=C["text_accent"],
        font=FONTS["ui_bold"])

    # ── Label ──────────────────────────────────────────────────────────────
    style.configure("TLabel",
        background=C["bg_base"], foreground=C["text_primary"],
        font=FONTS["ui"])
    style.configure("Header.TLabel",
        background=C["bg_base"], foreground=C["text_primary"],
        font=FONTS["ui_header"])
    style.configure("Title.TLabel",
        background=C["bg_base"], foreground=C["accent_bright"],
        font=FONTS["ui_title"])
    style.configure("Muted.TLabel",
        background=C["bg_base"], foreground=C["text_muted"],
        font=FONTS["ui_small"])
    style.configure("Success.TLabel",
        background=C["bg_base"], foreground=C["text_success"],
        font=FONTS["ui"])
    style.configure("Error.TLabel",
        background=C["bg_base"], foreground=C["text_error"],
        font=FONTS["ui"])
    style.configure("Warning.TLabel",
        background=C["bg_base"], foreground=C["text_warning"],
        font=FONTS["ui"])
    style.configure("Card.TLabel",
        background=C["bg_card"], foreground=C["text_primary"],
        font=FONTS["ui"])
    style.configure("Sidebar.TLabel",
        background=C["bg_sidebar"], foreground=C["text_secondary"],
        font=FONTS["ui"])

    # ── Button ─────────────────────────────────────────────────────────────
    style.configure("TButton",
        background=C["bg_elevated"], foreground=C["text_primary"],
        font=FONTS["ui"], borderwidth=1, relief="flat",
        focuscolor=C["accent"], padding=(12, 6))
    style.map("TButton",
        background=[("active", C["bg_hover"]), ("pressed", C["bg_active"]),
                    ("disabled", C["bg_surface"])],
        foreground=[("disabled", C["text_disabled"])],
        bordercolor=[("focus", C["border_focus"]), ("!focus", C["border"])])

    style.configure("Accent.TButton",
        background=C["accent_dim"], foreground=C["text_primary"],
        font=FONTS["ui_bold"], borderwidth=0, relief="flat", padding=(14, 7))
    style.map("Accent.TButton",
        background=[("active", C["accent"]), ("pressed", C["accent_bright"])])

    style.configure("Danger.TButton",
        background="#6B0020", foreground=C["text_primary"],
        font=FONTS["ui"], borderwidth=0, relief="flat", padding=(12, 6))
    style.map("Danger.TButton",
        background=[("active", C["text_error"]), ("pressed", "#FF6080")])

    style.configure("Ghost.TButton",
        background=C["bg_base"], foreground=C["text_secondary"],
        font=FONTS["ui"], borderwidth=1, relief="flat", padding=(10, 5))
    style.map("Ghost.TButton",
        background=[("active", C["bg_surface"])],
        foreground=[("active", C["text_primary"])])

    style.configure("Icon.TButton",
        background=C["bg_base"], foreground=C["text_primary"],
        font=FONTS["icon"], borderwidth=0, relief="flat", padding=(4, 4))
    style.map("Icon.TButton",
        background=[("active", C["bg_elevated"])])

    style.configure("Taskbar.TButton",
        background=C["taskbar_bg"], foreground=C["text_primary"],
        font=FONTS["ui"], borderwidth=0, relief="flat", padding=(8, 4))
    style.map("Taskbar.TButton",
        background=[("active", C["taskbar_btn"])])

    # ── Entry ──────────────────────────────────────────────────────────────
    style.configure("TEntry",
        fieldbackground=C["bg_input"], foreground=C["text_primary"],
        insertcolor=C["accent"], bordercolor=C["border"],
        selectbackground=C["accent_dim"], selectforeground=C["text_primary"],
        font=FONTS["ui"], padding=(8, 5))
    style.map("TEntry",
        bordercolor=[("focus", C["border_focus"]), ("!focus", C["border"])])

    style.configure("Terminal.TEntry",
        fieldbackground=C["term_bg"], foreground=C["term_fg"],
        insertcolor=C["term_cursor"], bordercolor=C["border"],
        font=FONTS["mono"], padding=(6, 4))

    style.configure("Search.TEntry",
        fieldbackground=C["bg_elevated"], foreground=C["text_primary"],
        insertcolor=C["accent"], bordercolor=C["border_bright"],
        font=FONTS["ui"], padding=(8, 5))

    # ── Combobox ───────────────────────────────────────────────────────────
    style.configure("TCombobox",
        fieldbackground=C["bg_input"], foreground=C["text_primary"],
        background=C["bg_elevated"], arrowcolor=C["accent"],
        bordercolor=C["border"], insertcolor=C["accent"],
        selectbackground=C["accent_dim"], selectforeground=C["text_primary"],
        font=FONTS["ui"], padding=(8, 5))
    style.map("TCombobox",
        bordercolor=[("focus", C["border_focus"])])

    # ── Scrollbar ──────────────────────────────────────────────────────────
    style.configure("TScrollbar",
        background=C["bg_surface"], troughcolor=C["bg_deep"],
        arrowcolor=C["text_muted"], borderwidth=0, relief="flat")
    style.map("TScrollbar",
        background=[("active", C["accent_dim"])])
    style.configure("Vertical.TScrollbar",
        width=8)
    style.configure("Horizontal.TScrollbar",
        arrowsize=8)

    # ── Notebook (tabs) ────────────────────────────────────────────────────
    style.configure("TNotebook",
        background=C["bg_base"], borderwidth=0, tabmargins=[0, 0, 0, 0])
    style.configure("TNotebook.Tab",
        background=C["bg_surface"], foreground=C["text_secondary"],
        font=FONTS["ui"], padding=(14, 6), borderwidth=0)
    style.map("TNotebook.Tab",
        background=[("selected", C["bg_card"]), ("active", C["bg_elevated"])],
        foreground=[("selected", C["text_primary"]), ("active", C["text_primary"])])

    # ── Treeview ───────────────────────────────────────────────────────────
    style.configure("Treeview",
        background=C["bg_input"], foreground=C["text_primary"],
        fieldbackground=C["bg_input"], borderwidth=0,
        font=FONTS["ui"], rowheight=26)
    style.configure("Treeview.Heading",
        background=C["bg_elevated"], foreground=C["text_accent"],
        font=FONTS["ui_bold"], borderwidth=0, relief="flat")
    style.map("Treeview",
        background=[("selected", C["accent_dim"])],
        foreground=[("selected", C["text_primary"])])
    style.map("Treeview.Heading",
        background=[("active", C["bg_hover"])])

    # ── Progressbar ───────────────────────────────────────────────────────
    style.configure("TProgressbar",
        background=C["accent"], troughcolor=C["bg_surface"],
        borderwidth=0, thickness=6)
    style.configure("Thin.TProgressbar",
        background=C["accent2"], troughcolor=C["bg_surface"],
        borderwidth=0, thickness=4)

    # ── Scale ─────────────────────────────────────────────────────────────
    style.configure("TScale",
        background=C["bg_base"], troughcolor=C["bg_surface"],
        sliderthickness=16, sliderrelief="flat")
    style.map("TScale",
        background=[("active", C["bg_base"])])

    # ── Checkbutton / Radiobutton ─────────────────────────────────────────
    style.configure("TCheckbutton",
        background=C["bg_base"], foreground=C["text_primary"],
        font=FONTS["ui"], indicatorcolor=C["accent"],
        focuscolor=C["bg_base"])
    style.map("TCheckbutton",
        background=[("active", C["bg_base"])])
    style.configure("TRadiobutton",
        background=C["bg_base"], foreground=C["text_primary"],
        font=FONTS["ui"], indicatorcolor=C["accent"],
        focuscolor=C["bg_base"])

    # ── Separator ─────────────────────────────────────────────────────────
    style.configure("TSeparator", background=C["border"])

    # ── Spinbox ───────────────────────────────────────────────────────────
    style.configure("TSpinbox",
        fieldbackground=C["bg_input"], foreground=C["text_primary"],
        arrowcolor=C["accent"], bordercolor=C["border"],
        font=FONTS["ui"], padding=(6, 4))

    return style


# ══════════════════════════════════════════════════════════════════════════════
#  Reusable UI Components
# ══════════════════════════════════════════════════════════════════════════════

class GlassFrame(tk.Frame):
    """A frame with a semi-transparent glass effect via canvas background."""
    def __init__(self, parent, radius=12, alpha=0.85, **kwargs):
        kwargs.setdefault("bg", C["bg_card"])
        kwargs.setdefault("highlightthickness", 1)
        kwargs.setdefault("highlightbackground", C["border"])
        super().__init__(parent, **kwargs)


class AccentLine(tk.Frame):
    """A thin horizontal accent line."""
    def __init__(self, parent, color=None, **kwargs):
        kwargs["height"] = 2
        kwargs["bg"] = color or C["accent"]
        kwargs["borderwidth"] = 0
        super().__init__(parent, **kwargs)


class Divider(ttk.Separator):
    def __init__(self, parent, **kwargs):
        super().__init__(parent, **kwargs)


class StatusDot(tk.Canvas):
    """A small colored status indicator dot."""
    COLORS = {
        "online":    "#3BFFD8",
        "offline":   "#FF4D6D",
        "idle":      "#FFD700",
        "busy":      "#FF6FBF",
        "unknown":   "#9999CC",
    }
    def __init__(self, parent, status="unknown", size=10, **kwargs):
        super().__init__(parent, width=size, height=size,
                         bg=parent.cget("bg"), highlightthickness=0, **kwargs)
        color = self.COLORS.get(status, self.COLORS["unknown"])
        self.create_oval(1, 1, size-1, size-1, fill=color, outline="")

    def set_status(self, status: str):
        color = self.COLORS.get(status, self.COLORS["unknown"])
        self.delete("all")
        s = int(self.cget("width"))
        self.create_oval(1, 1, s-1, s-1, fill=color, outline="")


class KuronoScrolledText(tk.Frame):
    """Text widget with scrollbar, dark themed."""
    def __init__(self, parent, font=None, bg=None, fg=None, **kwargs):
        super().__init__(parent, bg=C["bg_base"])
        bg = bg or C["bg_input"]
        fg = fg or C["text_primary"]
        font = font or FONTS["mono"]
        self.text = tk.Text(self, bg=bg, fg=fg, font=font,
            insertbackground=C["accent"], selectbackground=C["accent_dim"],
            relief="flat", borderwidth=0, padx=8, pady=6,
            wrap="none", **kwargs)
        self.vsb = ttk.Scrollbar(self, orient="vertical",
            command=self.text.yview)
        self.hsb = ttk.Scrollbar(self, orient="horizontal",
            command=self.text.xview)
        self.text.configure(yscrollcommand=self.vsb.set,
                            xscrollcommand=self.hsb.set)
        self.vsb.pack(side="right", fill="y")
        self.hsb.pack(side="bottom", fill="x")
        self.text.pack(side="left", fill="both", expand=True)

    def insert(self, *args, **kwargs):
        self.text.insert(*args, **kwargs)

    def get(self, *args, **kwargs):
        return self.text.get(*args, **kwargs)

    def delete(self, *args, **kwargs):
        self.text.delete(*args, **kwargs)

    def see(self, *args, **kwargs):
        self.text.see(*args, **kwargs)

    def configure(self, **kwargs):
        self.text.configure(**kwargs)


def make_tooltip(widget, text: str):
    """Attach a simple tooltip to a widget."""
    tip = None
    def enter(e):
        nonlocal tip
        tip = tk.Toplevel(widget)
        tip.wm_overrideredirect(True)
        tip.wm_geometry(f"+{e.x_root+12}+{e.y_root+6}")
        lbl = tk.Label(tip, text=text, bg=C["tooltip_bg"], fg=C["text_primary"],
                       font=FONTS["ui_small"], padx=8, pady=4,
                       relief="flat", borderwidth=1)
        lbl.pack()
    def leave(e):
        nonlocal tip
        if tip:
            tip.destroy()
            tip = None
    widget.bind("<Enter>", enter)
    widget.bind("<Leave>", leave)


def badge(parent, text: str, color: str = None, bg: str = None) -> tk.Label:
    """A small colored badge label."""
    return tk.Label(parent, text=text,
                    bg=bg or C["accent_dim"], fg=color or C["text_primary"],
                    font=FONTS["ui_small"], padx=6, pady=1,
                    relief="flat", borderwidth=0)
