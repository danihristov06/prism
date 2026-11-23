import os
import sys
import time
import json
import shutil
import hashlib
import tempfile
import threading
import subprocess
from pathlib import Path
import ctypes
from ctypes import wintypes
import queue
import uuid

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

import prism

# Enable DPI awareness on Windows
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

class ToolTip:
    """Simple tooltip implementation for toolbar and widgets."""
    def __init__(self, widget, text):
        self.widget = widget
        self.text = text
        self.tipwindow = None
        self.id = None
        self.widget.bind("<Enter>", self.enter)
        self.widget.bind("<Leave>", self.leave)

    def enter(self, event=None):
        self.schedule()

    def leave(self, event=None):
        self.unschedule()
        self.hide()

    def schedule(self):
        self.unschedule()
        self.id = self.widget.after(500, self.show)

    def unschedule(self):
        id_ = self.id
        self.id = None
        if id_:
            self.widget.after_cancel(id_)

    def show(self):
        if self.tipwindow or not self.text:
            return
        x = self.widget.winfo_rootx() + 20
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 5
        self.tipwindow = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry(f"+{x}+{y}")
        label = tk.Label(tw, text=self.text, background="#ffffcc", foreground="#000000",
                         relief="solid", borderwidth=1, font=("Segoe UI", 9))
        label.pack(ipadx=4, ipady=2)

    def hide(self):
        tw = self.tipwindow
        self.tipwindow = None
        if tw:
            tw.destroy()


class PrismArchiverApp:
    def __init__(self, root):
        self.root = root
        self.root.title("PRISM Archiver Studio v2.0")
        self.root.minsize(1050, 720)
        
        # Thread-safe UI queue
        self.ui_queue = queue.Queue()
        self.root.after(50, self.poll_ui_queue)
        
        # Load settings & geometry
        self.settings_file = Path.home() / ".prism_gui.json"
        self.load_settings()
        
        # Apply theme
        self.setup_theme()
        
        # State variables
        self.loaded_archive_path = None
        self.all_archive_entries = []
        self.archive_meta = {}
        self.archive_pmet = None
        self.current_folder_prefix = ""
        self.nav_history = [""]
        self.nav_index = 0
        self.log_visible = self.settings.get("show_log_on_start", False)
        self.recent_archives = self.settings.get("recent_archives", [])
        self.running_process = None
        self.cancel_requested = False
        self.is_busy = False
        self._last_auto_out = ""
        self._preview_cache = {}
        self.custom_bench_file = Path.home() / ".prism_custom_benchmarks.json"
        self.custom_benchmarks = []
        self.custom_bench_baseline_id = None
        self._dnd_hwnd = None
        self._dnd_wndproc = None
        self._orig_wndproc = None
        self._set_wndproc = None
        
        # Build UI components
        self.build_menu_bar()
        self.build_action_toolbar()
        
        # Main Workspace Container (Notebook)
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        self.notebook.bind("<<NotebookTabChanged>>", self.on_tab_changed)
        
        self.tab_explorer = ttk.Frame(self.notebook)
        self.tab_compress = ttk.Frame(self.notebook)
        self.tab_benchmark = ttk.Frame(self.notebook)
        
        self.notebook.add(self.tab_explorer, text="Archive Explorer")
        self.notebook.add(self.tab_compress, text="Pack / Compress Queue")
        self.notebook.add(self.tab_benchmark, text="Benchmark & Version Diff")
        
        self.build_explorer_tab()
        self.build_compress_tab()
        self.build_benchmark_tab()
        
        # Collapsible Activity Log Drawer
        self.log_frame = ttk.Frame(self.root)
        self.log_text = scrolledtext.ScrolledText(
            self.log_frame, height=7,
            bg=self.log_bg, fg=self.fg_light, insertbackground=self.fg_light,
            borderwidth=0, font=("Consolas", 9)
        )
        self.log_text.pack(fill=tk.BOTH, expand=True, padx=5, pady=2)
        if self.log_visible:
            self.log_frame.pack(fill=tk.X, side=tk.BOTTOM, padx=5, pady=2)
        
        # Mandatory Persistent Bottom Status Bar
        self.build_status_bar()
        
        # Bind global shortcuts
        self.bind_shortcuts()
        
        # Initial status update
        self.update_status(left="Ready", mid="No archive loaded")
        
        # Register native Windows Drag & Drop (WM_DROPFILES)
        self.setup_native_drag_drop()

    def poll_ui_queue(self):
        try:
            while True:
                try:
                    callback = self.ui_queue.get_nowait()
                    if callable(callback):
                        callback()
                except queue.Empty:
                    break
        except Exception:
            pass
        try:
            if self.root.winfo_exists():
                self.root.after(50, self.poll_ui_queue)
        except tk.TclError:
            pass

    def load_settings(self):
        default_settings = {
            "theme": "dark",
            "default_preset": "Normal - Default",
            "default_threads": "Auto",
            "default_output": "",
            "size_unit": "MB (1024)",
            "embed_meta": True,
            "auto_verify": True,
            "confirm_overwrite": True,
            "show_log_on_start": False,
            "recent_archives": []
        }
        self.settings = default_settings.copy()
        if self.settings_file.exists():
            try:
                with open(self.settings_file, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    self.settings.update(data)
                    if "geometry" in self.settings:
                        self.root.geometry(self.settings["geometry"])
            except Exception:
                pass

    def _get_unit_spec(self):
        """Return (k_div, m_div, g_div, k_lbl, m_lbl, g_lbl, speed_lbl) for active size_unit."""
        unit = self.settings.get("size_unit", "MB (1024)")
        if unit == "MB (SI 1000)":
            return (1000.0, 1000000.0, 1000000000.0, "KB", "MB", "GB", "MB/s")
        if unit == "MiB (IEC 1024)":
            return (1024.0, 1048576.0, 1073741824.0, "KiB", "MiB", "GiB", "MiB/s")
        return (1024.0, 1048576.0, 1073741824.0, "KB", "MB", "GB", "MB/s")

    def _unit_labels(self):
        """Return (m_div, k_lbl, m_lbl, g_lbl, speed_lbl) for active size_unit."""
        _, m_div, _, k_lbl, m_lbl, g_lbl, speed_lbl = self._get_unit_spec()
        return m_div, k_lbl, m_lbl, g_lbl, speed_lbl

    def format_speed_from_bytes(self, size_bytes, elapsed_sec):
        """Format throughput from byte count and elapsed seconds using active unit."""
        m_div, _, _, _, speed_lbl = self._unit_labels()
        if elapsed_sec is None or elapsed_sec <= 0 or size_bytes is None or size_bytes <= 0:
            return f"0.00 {speed_lbl}"
        return f"{(float(size_bytes) / m_div) / float(elapsed_sec):.2f} {speed_lbl}"

    def format_speed_mibs(self, mib_s):
        """Convert an IEC MiB/s float to active speed unit string."""
        m_div, _, _, _, speed_lbl = self._unit_labels()
        if mib_s is None:
            return f"0.00 {speed_lbl}"
        val = float(mib_s) * 1048576.0 / m_div
        return f"{val:.2f} {speed_lbl}"

    def _get_corpus_metric_values(self):
        """Return unit-aware metric combobox labels for the corpus benchmark chart."""
        _, _, m_lbl, _, speed_lbl = self._unit_labels()
        return [
            f"Compressed Size ({m_lbl})",
            f"Encode Speed ({speed_lbl})",
            f"Decode Speed ({speed_lbl})",
        ]

    def _refresh_unit_displays(self):
        """Refresh all visible unit-formatted strings immediately after unit setting changes."""
        _, _, _, _, speed_lbl = self._unit_labels()
        if hasattr(self, "comp_summary_label"):
            self.update_comp_summary()
        if hasattr(self, "bench_metric_combo") and hasattr(self, "bench_metric_var"):
            new_vals = self._get_corpus_metric_values()
            cur_metric = self.bench_metric_var.get()
            self.bench_metric_combo.configure(values=new_vals)
            if "Encode Speed" in cur_metric:
                self.bench_metric_var.set(new_vals[1])
            elif "Decode Speed" in cur_metric:
                self.bench_metric_var.set(new_vals[2])
            else:
                self.bench_metric_var.set(new_vals[0])
        if hasattr(self, "bench_tree"):
            self.load_benchmark_data()
        if hasattr(self, "custom_bench_tree"):
            self.refresh_custom_benchmarks_ui()
        if self.loaded_archive_path:
            if hasattr(self, "exp_badges_container"):
                self._render_explorer_badges()
            if hasattr(self, "file_tree"):
                self.populate_file_tree(self.current_folder_prefix)
        if not self.is_busy and hasattr(self, "status_speed_lbl"):
            self.status_speed_lbl.config(text=f"0.00 {speed_lbl}")

    def save_settings(self):
        try:
            self.settings["geometry"] = self.root.geometry()
            self.settings["recent_archives"] = self.recent_archives[:10]
            with open(self.settings_file, "w", encoding="utf-8") as f:
                json.dump(self.settings, f, indent=2)
        except Exception:
            pass

    def setup_theme(self):
        self.style = ttk.Style()
        self.style.theme_use("clam")
        
        is_dark = self.settings.get("theme", "dark") == "dark"
        if is_dark:
            self.bg_dark = "#1e1e1e"
            self.bg_card = "#2d2d2d"
            self.fg_light = "#d4d4d4"
            self.fg_muted = "#9e9e9e"
            self.accent = "#007acc"
            self.accent_hover = "#1c97ea"
            self.log_bg = "#181818"
            self.chart_bg = "#222222"
            self.chart_grid = "#333333"
            self.chart_axis = "#555555"
            self.chart_text = "#cccccc"
            self.win_bg = "#1e3a29"
            self.win_fg = "#a3e635"
            field_bg = "#252526"
            select_bg = "#007acc"
            heading_bg = "#333333"
            trough_bg = "#181818"
            border_col = "#3c3c3c"
        else:
            self.bg_dark = "#f3f3f3"
            self.bg_card = "#ffffff"
            self.fg_light = "#1e1e1e"
            self.fg_muted = "#555555"
            self.accent = "#0066cc"
            self.accent_hover = "#0052a3"
            self.log_bg = "#fafafa"
            self.chart_bg = "#ffffff"
            self.chart_grid = "#e5e5e5"
            self.chart_axis = "#888888"
            self.chart_text = "#222222"
            self.win_bg = "#e6f4ea"
            self.win_fg = "#137333"
            field_bg = "#ffffff"
            select_bg = "#0066cc"
            heading_bg = "#e1e1e1"
            trough_bg = "#dcdcdc"
            border_col = "#cccccc"
            
        self.field_bg = field_bg
        self.heading_bg = heading_bg
        self.trough_bg = trough_bg
        self.border_col = border_col
        self.root.configure(bg=self.bg_dark)
        
        self.style.configure(".", background=self.bg_dark, foreground=self.fg_light, font=("Segoe UI", 10))
        self.style.configure("TNotebook", background=self.bg_dark, borderwidth=0)
        self.style.configure("TNotebook.Tab", background=self.bg_card, foreground=self.fg_light, padding=[14, 8], font=("Segoe UI", 10, "bold"))
        self.style.map("TNotebook.Tab", background=[("selected", self.accent)], foreground=[("selected", "#ffffff")])
        
        self.style.configure("TFrame", background=self.bg_dark)
        self.style.configure("Card.TFrame", background=self.bg_card, relief="flat")
        self.style.configure("CardBorder.TFrame", background=field_bg, relief="solid", borderwidth=1, bordercolor=border_col)
        self.style.configure("SubCard.TFrame", background=field_bg, relief="flat", borderwidth=0)
        self.style.configure("TLabel", background=self.bg_dark, foreground=self.fg_light)
        self.style.configure("Muted.TLabel", background=self.bg_dark, foreground=self.fg_muted)
        self.style.configure("Card.TLabel", background=self.bg_card, foreground=self.fg_light)
        self.style.configure("CardMuted.TLabel", background=self.bg_card, foreground=self.fg_muted)
        self.style.configure("SubCard.TLabel", background=field_bg, foreground=self.fg_light)
        self.style.configure("SubCardMuted.TLabel", background=field_bg, foreground=self.fg_muted)
        
        self.style.configure("TButton", background=heading_bg, foreground=self.fg_light, bordercolor=border_col, borderwidth=1, focusthickness=2, focuscolor="none", padding=[8, 4])
        self.style.map("TButton", background=[("active", self.accent), ("disabled", self.bg_dark)], foreground=[("active", "#ffffff"), ("disabled", self.fg_muted)])
        self.style.configure("Accent.TButton", background=self.accent, foreground="#ffffff", font=("Segoe UI", 10, "bold"), padding=[10, 5])
        self.style.map("Accent.TButton", background=[("active", self.accent_hover), ("disabled", heading_bg)], foreground=[("disabled", self.fg_muted)])
        self.style.configure("Danger.TButton", background="#b71c1c", foreground="#ffffff", font=("Segoe UI", 9, "bold"), padding=[8, 3])
        self.style.map("Danger.TButton", background=[("active", "#d32f2f")])
        
        # Match checkbutton & radiobutton backgrounds to parent card / frame (including hover/disabled states)
        self.style.configure("TCheckbutton", background=self.bg_dark, foreground=self.fg_light, indicatorbackground=field_bg, indicatorforeground=self.fg_light)
        self.style.map("TCheckbutton", background=[("active", self.bg_dark), ("disabled", self.bg_dark)], foreground=[("active", self.fg_light)], indicatorbackground=[("selected", self.accent), ("active", field_bg)])
        self.style.configure("Card.TCheckbutton", background=self.bg_card, foreground=self.fg_light, indicatorbackground=field_bg, indicatorforeground=self.fg_light)
        self.style.map("Card.TCheckbutton", background=[("active", self.bg_card), ("disabled", self.bg_card)], foreground=[("active", self.fg_light)], indicatorbackground=[("selected", self.accent), ("active", field_bg)])
        
        self.style.configure("TRadiobutton", background=self.bg_dark, foreground=self.fg_light, indicatorbackground=field_bg, indicatorforeground=self.fg_light)
        self.style.map("TRadiobutton", background=[("active", self.bg_dark), ("disabled", self.bg_dark)], foreground=[("active", self.fg_light)], indicatorbackground=[("selected", self.accent), ("active", field_bg)])
        self.style.configure("Card.TRadiobutton", background=self.bg_card, foreground=self.fg_light, indicatorbackground=field_bg, indicatorforeground=self.fg_light)
        self.style.map("Card.TRadiobutton", background=[("active", self.bg_card), ("disabled", self.bg_card)], foreground=[("active", self.fg_light)], indicatorbackground=[("selected", self.accent), ("active", field_bg)])
        
        # Progressbar high-contrast style (override clam lightcolor/darkcolor/bordercolor)
        self.style.configure(
            "Horizontal.TProgressbar",
            troughcolor=trough_bg,
            background=self.accent,
            bordercolor=border_col,
            lightcolor=self.accent,
            darkcolor=self.accent,
            borderwidth=1,
            thickness=12
        )
        
        # Scrollbar & Panedwindow
        self.style.configure("Vertical.TScrollbar", background=heading_bg, troughcolor=trough_bg, bordercolor=border_col, arrowcolor=self.fg_light)
        self.style.configure("Horizontal.TScrollbar", background=heading_bg, troughcolor=trough_bg, bordercolor=border_col, arrowcolor=self.fg_light)
        self.style.configure("TPanedwindow", background=self.bg_dark)
        self.style.configure("Sash", sashthickness=5, gripcount=0)
        self.style.configure("TSeparator", background=border_col)
        
        # Combobox, Entry, Treeview
        self.style.configure("TCombobox", fieldbackground=field_bg, background=heading_bg, foreground=self.fg_light, selectbackground=select_bg, selectforeground="#ffffff", arrowcolor=self.fg_light, bordercolor=border_col)
        self.style.map("TCombobox", fieldbackground=[("readonly", field_bg)], foreground=[("readonly", self.fg_light)], selectbackground=[("readonly", select_bg)], selectforeground=[("readonly", "#ffffff")])
        
        self.style.configure("TEntry", fieldbackground=field_bg, foreground=self.fg_light, insertcolor=self.fg_light, bordercolor=border_col)
        self.style.map("TEntry", fieldbackground=[("readonly", field_bg)], foreground=[("readonly", self.fg_light)])
        
        self.style.configure("Treeview", background=self.bg_card, fieldbackground=field_bg, foreground=self.fg_light, borderwidth=0, rowheight=24)
        self.style.configure("Treeview.Heading", background=heading_bg, foreground=self.fg_light, bordercolor=border_col, font=("Segoe UI", 10, "bold"))
        self.style.map("Treeview", background=[("selected", select_bg)], foreground=[("selected", "#ffffff")])
        self.style.map("Treeview.Heading", background=[("active", self.accent)], foreground=[("active", "#ffffff")])

    def apply_dynamic_theme_colors(self):
        """Update non-ttk widgets when theme switches between dark and light."""
        if hasattr(self, "log_text"):
            self.log_text.configure(bg=self.log_bg, fg=self.fg_light, insertbackground=self.fg_light)
        if hasattr(self, "exp_menu"):
            self.exp_menu.configure(bg=self.bg_card, fg=self.fg_light, activebackground=self.accent, activeforeground="#ffffff")
        if hasattr(self, "chart_canvas"):
            self.chart_canvas.configure(bg=self.chart_bg)
            self.draw_benchmark_chart()
        if hasattr(self, "bench_tree"):
            self.bench_tree.tag_configure("win_all", background=self.win_bg, foreground=self.win_fg)
        if hasattr(self, "custom_chart_canvas"):
            self.custom_chart_canvas.configure(bg=self.chart_bg)
            self.draw_custom_benchmark_chart()
        if hasattr(self, "custom_bench_tree"):
            self.custom_bench_tree.tag_configure("baseline", background=self.win_bg, foreground=self.win_fg)

    def bind_shortcuts(self):
        self.root.bind("<Control-o>", lambda e: self.exp_open_archive())
        self.root.bind("<Control-n>", lambda e: self.notebook.select(self.tab_compress))
        self.root.bind("<Control-e>", lambda e: self.exp_extract_all())
        self.root.bind("<Control-t>", lambda e: self.exp_verify())
        self.root.bind("<F5>", lambda e: self.load_archive_details() if self.loaded_archive_path else None)
        self.root.bind("<Control-f>", lambda e: self.focus_search())
        self.root.bind("<Control-a>", lambda e: self.select_all_files())
        self.root.bind("<Escape>", lambda e: self.cancel_current_operation())
        self.root.bind("<F1>", lambda e: self.show_about_dialog())
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def on_close(self):
        if self._dnd_hwnd and self._orig_wndproc and self._set_wndproc:
            try:
                self._set_wndproc(self._dnd_hwnd, -4, self._orig_wndproc)
            except Exception:
                pass
            self._dnd_hwnd = None
            self._orig_wndproc = None
        self.save_settings()
        self.root.destroy()

    def setup_native_drag_drop(self):
        """Register native Win32 WM_DROPFILES drag-and-drop without external packages."""
        if sys.platform != "win32":
            return
        try:
            self.root.update_idletasks()
            try:
                frame_hex = self.root.wm_frame()
                hwnd = int(frame_hex, 16) if frame_hex and frame_hex != "0x0" else 0
            except Exception:
                hwnd = 0
            if not hwnd:
                hwnd = self.root.winfo_id()
            if not hwnd:
                return

            user32 = ctypes.windll.user32
            shell32 = ctypes.windll.shell32
            is_64bit = ctypes.sizeof(ctypes.c_void_p) == 8

            lresult_t = ctypes.c_ssize_t
            wndproc_t = ctypes.WINFUNCTYPE(lresult_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

            set_wndproc = user32.SetWindowLongPtrW if is_64bit else user32.SetWindowLongW
            set_wndproc.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
            set_wndproc.restype = ctypes.c_void_p

            user32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            user32.CallWindowProcW.restype = lresult_t

            shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
            shell32.DragQueryFileW.argtypes = [wintypes.HANDLE, wintypes.UINT, wintypes.LPWSTR, wintypes.UINT]
            shell32.DragQueryFileW.restype = wintypes.UINT
            shell32.DragFinish.argtypes = [wintypes.HANDLE]

            WM_DROPFILES = 0x0233
            GWLP_WNDPROC = -4

            def _wndproc(h, msg, wparam, lparam):
                if msg == WM_DROPFILES:
                    hdrop = wintypes.HANDLE(wparam)
                    count = shell32.DragQueryFileW(hdrop, 0xFFFFFFFF, None, 0)
                    paths = []
                    for i in range(count):
                        length = shell32.DragQueryFileW(hdrop, i, None, 0)
                        buf = ctypes.create_unicode_buffer(length + 1)
                        shell32.DragQueryFileW(hdrop, i, buf, length + 1)
                        if buf.value:
                            paths.append(buf.value)
                    shell32.DragFinish(hdrop)
                    if paths:
                        self.ui_queue.put(lambda p=paths: self.handle_dropped_paths(p))
                    return 0
                return user32.CallWindowProcW(self._orig_wndproc, h, msg, wparam, lparam)

            # Keep strong reference on self so callback is never garbage-collected
            self._dnd_wndproc = wndproc_t(_wndproc)
            self._set_wndproc = set_wndproc
            self._dnd_hwnd = hwnd
            shell32.DragAcceptFiles(hwnd, True)
            self._orig_wndproc = set_wndproc(hwnd, GWLP_WNDPROC, ctypes.cast(self._dnd_wndproc, ctypes.c_void_p))
        except Exception as e:
            self.log_activity(f"Native drag-and-drop registration skipped: {e}")

    def handle_dropped_paths(self, paths):
        """Route dropped files/folders to Explorer (single .prism) or Pack Queue."""
        valid = [os.path.abspath(p) for p in paths if p and os.path.exists(p)]
        if not valid:
            return
        if len(valid) == 1 and os.path.isfile(valid[0]) and valid[0].lower().endswith(".prism"):
            self.loaded_archive_path = valid[0]
            self.add_recent_archive(valid[0])
            self.notebook.select(self.tab_explorer)
            self.load_archive_details()
            return

        added = 0
        for p in valid:
            if os.path.isdir(p):
                total_size = sum(f.stat().st_size for f in Path(p).rglob("*") if f.is_file())
                self.comp_tree.insert("", tk.END, values=(p, "Folder", total_size))
                added += 1
            elif os.path.isfile(p):
                sz = os.path.getsize(p)
                self.comp_tree.insert("", tk.END, values=(p, "File", sz))
                added += 1
        if added:
            self.update_comp_summary()
            self.auto_set_output_default()
            self.notebook.select(self.tab_compress)
            self.log_activity(f"Added {added} dropped item(s) to Pack Queue.")

    def on_tab_changed(self, event):
        current_tab = self.notebook.select()
        if current_tab == str(self.tab_explorer):
            if self.loaded_archive_path:
                self.update_explorer_status()
            else:
                self.update_status(mid="No archive loaded")
        elif current_tab == str(self.tab_compress):
            self.update_status(mid="Pack Queue Ready")
        elif current_tab == str(self.tab_benchmark):
            self.update_status(mid="Benchmark & Version Diff Suite")

    # ==================== MENU BAR & TOOLBAR ====================
    def build_menu_bar(self):
        menu_bg = self.bg_card
        menu_fg = self.fg_light
        menubar = tk.Menu(self.root, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff", borderwidth=0)
        
        file_menu = tk.Menu(menubar, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        file_menu.add_command(label="Open .prism Archive...\tCtrl+O", command=self.exp_open_archive)
        
        self.recent_menu = tk.Menu(file_menu, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        file_menu.add_cascade(label="Recent Archives", menu=self.recent_menu)
        self.update_recent_menu()
        
        file_menu.add_command(label="Close Archive", command=self.close_archive)
        file_menu.add_separator()
        file_menu.add_command(label="Exit\tAlt+F4", command=self.on_close)
        menubar.add_cascade(label="File", menu=file_menu)
        
        edit_menu = tk.Menu(menubar, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        edit_menu.add_command(label="Select All\tCtrl+A", command=self.select_all_files)
        edit_menu.add_command(label="Invert Selection", command=self.invert_selection)
        edit_menu.add_command(label="Find / Filter\tCtrl+F", command=self.focus_search)
        menubar.add_cascade(label="Edit", menu=edit_menu)
        
        cmd_menu = tk.Menu(menubar, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        cmd_menu.add_command(label="Add Files / Folders", command=self.comp_add_files)
        cmd_menu.add_command(label="Compress / Pack\tCtrl+N", command=lambda: self.notebook.select(self.tab_compress))
        cmd_menu.add_command(label="Extract All...\tCtrl+E", command=self.exp_extract_all)
        cmd_menu.add_command(label="Extract Selected...", command=self.exp_extract_selected)
        cmd_menu.add_separator()
        cmd_menu.add_command(label="Test / Verify Archive\tCtrl+T", command=self.exp_verify)
        cmd_menu.add_command(label="Archive Info", command=self.show_archive_info_dialog)
        cmd_menu.add_separator()
        cmd_menu.add_command(label="Cancel Operation\tEsc", command=self.cancel_current_operation)
        menubar.add_cascade(label="Commands", menu=cmd_menu)
        
        view_menu = tk.Menu(menubar, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        view_menu.add_command(label="Toggle Activity Log", command=self.toggle_activity_log)
        view_menu.add_command(label="Refresh Explorer\tF5", command=lambda: self.load_archive_details() if self.loaded_archive_path else None)
        view_menu.add_separator()
        view_menu.add_command(label="Toggle Light/Dark Theme", command=self.toggle_theme)
        menubar.add_cascade(label="View", menu=view_menu)
        
        tools_menu = tk.Menu(menubar, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        tools_menu.add_command(label="Benchmark & Diff", command=lambda: self.notebook.select(self.tab_benchmark))
        tools_menu.add_command(label="Settings...", command=self.show_settings_dialog)
        menubar.add_cascade(label="Tools", menu=tools_menu)
        
        help_menu = tk.Menu(menubar, tearoff=0, bg=menu_bg, fg=menu_fg, activebackground=self.accent, activeforeground="#ffffff")
        help_menu.add_command(label="About PRISM Archiver\tF1", command=self.show_about_dialog)
        menubar.add_cascade(label="Help", menu=help_menu)
        
        self.root.config(menu=menubar)

    def update_recent_menu(self):
        self.recent_menu.delete(0, tk.END)
        if not self.recent_archives:
            self.recent_menu.add_command(label="(No Recent Archives)", state=tk.DISABLED)
        else:
            for path in self.recent_archives:
                if os.path.exists(path):
                    self.recent_menu.add_command(label=path, command=lambda p=path: self.open_recent_archive(p))

    def open_recent_archive(self, path):
        if os.path.exists(path):
            self.loaded_archive_path = path
            self.add_recent_archive(path)
            self.load_archive_details()
        else:
            messagebox.showerror("Error", f"File not found: {path}")

    def add_recent_archive(self, path):
        if path in self.recent_archives:
            self.recent_archives.remove(path)
        self.recent_archives.insert(0, path)
        self.update_recent_menu()

    def toggle_theme(self):
        current = self.settings.get("theme", "dark")
        self.settings["theme"] = "light" if current == "dark" else "dark"
        self.setup_theme()
        self.build_menu_bar()
        self.apply_dynamic_theme_colors()
        theme_label = self.settings["theme"].capitalize()
        self.log_activity(f"Switched theme to {theme_label}.")
        self.update_status(left=f"Theme: {theme_label}")

    def select_all_files(self):
        current_tab = self.notebook.select()
        if current_tab == str(self.tab_compress):
            items = self.comp_tree.get_children()
            if items:
                self.comp_tree.selection_set(items)
        else:
            items = self.file_tree.get_children()
            if items:
                self.file_tree.selection_set(items)
                self.on_file_select()

    def invert_selection(self):
        current_tab = self.notebook.select()
        tree = self.comp_tree if current_tab == str(self.tab_compress) else self.file_tree
        all_items = set(tree.get_children())
        cur_sel = set(tree.selection())
        new_sel = list(all_items - cur_sel)
        tree.selection_set(new_sel)
        if tree is self.file_tree:
            self.on_file_select()

    def focus_search(self):
        self.notebook.select(self.tab_explorer)
        if hasattr(self, "search_entry_widget"):
            self.search_entry_widget.focus_set()
            self.search_entry_widget.select_range(0, tk.END)

    def build_action_toolbar(self):
        tb = ttk.Frame(self.root, style="Card.TFrame")
        tb.pack(fill=tk.X, side=tk.TOP, padx=5, pady=2)
        
        btn_defs = [
            ("➕ Add", self.comp_add_files, "Add files or folders to pack queue"),
            ("📁 Folder", self.comp_add_folder, "Add folder to pack queue"),
            ("🗜 Compress", lambda: self.notebook.select(self.tab_compress), "Switch to compression tab"),
            None,
            ("📂 Open", self.exp_open_archive, "Open .prism archive"),
            ("📤 Extract", self.exp_extract_all, "Extract archive contents"),
            ("✔ Test", self.exp_verify, "Verify archive integrity"),
            ("ℹ Info", self.show_archive_info_dialog, "Show detailed archive metadata"),
            None,
            ("📊 Benchmark", lambda: self.notebook.select(self.tab_benchmark), "Switch to benchmark tab"),
        ]
        
        self.toolbar_buttons = []
        for item in btn_defs:
            if item is None:
                ttk.Separator(tb, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=6, pady=4)
            else:
                text, cmd, tooltip = item
                b = ttk.Button(tb, text=text, command=cmd)
                b.pack(side=tk.LEFT, padx=3, pady=4)
                ToolTip(b, tooltip)
                if "Extract" in text or "Test" in text or "Info" in text:
                    self.toolbar_buttons.append(b)
        
        self.update_toolbar_states()

    def update_toolbar_states(self):
        state = tk.NORMAL if self.loaded_archive_path else tk.DISABLED
        for b in self.toolbar_buttons:
            try:
                b.config(state=state)
            except Exception:
                pass

    # ==================== STATUS BAR ====================
    def build_status_bar(self):
        sb = ttk.Frame(self.root)
        sb.pack(fill=tk.X, side=tk.BOTTOM)
        
        self.status_left_lbl = ttk.Label(sb, text="Ready", font=("Segoe UI", 9))
        self.status_left_lbl.pack(side=tk.LEFT, padx=10, pady=4)
        
        self.status_mid_lbl = ttk.Label(sb, text="No archive loaded", font=("Segoe UI", 9))
        self.status_mid_lbl.pack(side=tk.LEFT, padx=20, pady=4)
        
        right_frame = ttk.Frame(sb)
        right_frame.pack(side=tk.RIGHT, padx=10, pady=4)
        
        self.cancel_btn = ttk.Button(right_frame, text="Stop / Cancel", style="Danger.TButton", command=self.cancel_current_operation, state=tk.DISABLED)
        # Initially hidden / disabled when idle
        self.cancel_btn.pack_forget()
        
        _, _, _, _, speed_lbl = self._unit_labels()
        self.status_speed_lbl = ttk.Label(right_frame, text=f"0.00 {speed_lbl}", font=("Consolas", 9))
        self.status_speed_lbl.pack(side=tk.LEFT, padx=5)
        
        self.status_timer_lbl = ttk.Label(right_frame, text="0.00s", font=("Consolas", 9))
        self.status_timer_lbl.pack(side=tk.LEFT, padx=5)
        
        self.status_progress = ttk.Progressbar(right_frame, style="Horizontal.TProgressbar", mode="indeterminate", length=120)
        self.status_progress.pack(side=tk.LEFT, padx=5)

    def update_status(self, left=None, mid=None):
        if left:
            self.status_left_lbl.config(text=left)
        if mid:
            self.status_mid_lbl.config(text=mid)

    def log_activity(self, msg):
        timestamp = time.strftime("[%H:%M:%S] ")
        self.log_text.insert(tk.END, timestamp + msg + "\n")
        self.log_text.see(tk.END)

    def toggle_activity_log(self):
        if self.log_visible:
            self.log_frame.pack_forget()
            self.log_visible = False
        else:
            self.log_frame.pack(fill=tk.X, side=tk.BOTTOM, before=self.root.nametowidget(self.root.winfo_children()[-1]), padx=5, pady=2)
            self.log_visible = True

    def cancel_current_operation(self):
        if not self.is_busy and not self.running_process:
            return
        self.cancel_requested = True
        if self.running_process:
            try:
                self.running_process.terminate()
                self.log_activity("User cancelled current operation.")
            except Exception:
                pass
            self.running_process = None
        self.set_busy(False)
        if hasattr(self, "comp_btn"):
            self.comp_btn.config(state=tk.NORMAL)
        self.update_status(left="Cancelled", mid="Operation terminated by user")

    def start_busy(self, message=None, indeterminate=True):
        if message:
            self.update_status(left=message)
        self.set_busy(True, indeterminate=indeterminate)

    def stop_busy(self):
        self.set_busy(False)

    def set_busy(self, busy, indeterminate=True):
        self.is_busy = busy
        if busy:
            self.cancel_btn.pack(side=tk.LEFT, padx=5, before=self.status_speed_lbl)
            self.cancel_btn.config(state=tk.NORMAL)
            if indeterminate:
                self.status_progress.config(mode="indeterminate")
                self.status_progress.start(10)
        else:
            self.status_progress.stop()
            if str(self.status_progress.cget("mode")) == "determinate":
                self.status_progress.config(value=0)
            self.cancel_btn.pack_forget()
            self.cancel_btn.config(state=tk.DISABLED)

    # ==================== TAB 1: ARCHIVE EXPLORER ====================
    def build_explorer_tab(self):
        frame = self.tab_explorer
        
        # Empty state placeholder frame
        self.empty_state_frame = ttk.Frame(frame)
        self.empty_state_lbl = ttk.Label(
            self.empty_state_frame,
            text="📂 Open a .prism archive or Drag & Drop files/archives anywhere here\n(Drop a .prism file to explore, or files/folders to pack)",
            font=("Segoe UI", 14, "bold"),
            justify=tk.CENTER
        )
        self.empty_state_lbl.pack(expand=True, pady=100)
        self.empty_state_frame.pack(fill=tk.BOTH, expand=True)
        
        # Active Explorer Content Frame
        self.explorer_content_frame = ttk.Frame(frame)
        
        # Top Address / Nav bar with full unclipped button labels
        top_bar = ttk.Frame(self.explorer_content_frame)
        top_bar.pack(fill=tk.X, padx=5, pady=5)
        
        self.btn_back = ttk.Button(top_bar, text="Back", command=self.exp_go_back)
        self.btn_back.pack(side=tk.LEFT, padx=2)
        self.btn_forward = ttk.Button(top_bar, text="Forward", command=self.exp_go_forward)
        self.btn_forward.pack(side=tk.LEFT, padx=2)
        self.btn_up = ttk.Button(top_bar, text="Up", command=self.exp_go_up)
        self.btn_up.pack(side=tk.LEFT, padx=2)
        self.btn_refresh = ttk.Button(top_bar, text="Refresh", command=lambda: self.load_archive_details() if self.loaded_archive_path else None)
        self.btn_refresh.pack(side=tk.LEFT, padx=2)
        
        ttk.Label(top_bar, text="Path:").pack(side=tk.LEFT, padx=(10, 2))
        self.exp_path_var = tk.StringVar(value="Archive:\\")
        path_entry = ttk.Entry(top_bar, textvariable=self.exp_path_var, state="readonly", width=35)
        path_entry.pack(side=tk.LEFT, padx=2, fill=tk.X, expand=True)
        
        ttk.Label(top_bar, text="Search (Ctrl+F):").pack(side=tk.LEFT, padx=(15, 2))
        self.exp_search_var = tk.StringVar()
        self.exp_search_var.trace_add("write", lambda *args: self.exp_filter_tree())
        self.search_entry_widget = ttk.Entry(top_bar, textvariable=self.exp_search_var, width=18)
        self.search_entry_widget.pack(side=tk.LEFT, padx=2)
        
        self.match_count_lbl = ttk.Label(top_bar, text="", font=("Segoe UI", 8, "italic"))
        self.match_count_lbl.pack(side=tk.LEFT, padx=5)
        
        # Pipeline badges & status pill bar
        badges_bar = ttk.Frame(self.explorer_content_frame, style="Card.TFrame")
        badges_bar.pack(fill=tk.X, padx=5, pady=2)
        
        self.exp_verify_pill = tk.Label(badges_bar, text="UNCHECKED", bg="#555555", fg="#ffffff", font=("Segoe UI", 9, "bold"), padx=8, pady=3)
        self.exp_verify_pill.pack(side=tk.LEFT, padx=8, pady=5)
        
        self.exp_badges_container = ttk.Frame(badges_bar, style="Card.TFrame")
        self.exp_badges_container.pack(side=tk.LEFT, padx=5, pady=5)
        
        # Split Dual-Pane View
        paned = ttk.PanedWindow(self.explorer_content_frame, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=2)
        
        # Left Pane: Folder Tree
        left_frame = ttk.Frame(paned)
        paned.add(left_frame, weight=1)
        
        ttk.Label(left_frame, text="Folders", font=("Segoe UI", 10, "bold")).pack(anchor=tk.W, padx=5, pady=2)
        self.folder_tree = ttk.Treeview(left_frame, show="tree", selectmode="browse")
        f_scrollbar = ttk.Scrollbar(left_frame, orient=tk.VERTICAL, command=self.folder_tree.yview)
        self.folder_tree.configure(yscrollcommand=f_scrollbar.set)
        self.folder_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        f_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.folder_tree.bind("<<TreeviewSelect>>", self.on_folder_select)
        
        # Right Pane: File List Treeview (pipeline hidden unless per-entry metadata exists)
        right_frame = ttk.Frame(paned)
        paned.add(right_frame, weight=3)
        
        ttk.Label(right_frame, text="Contents", font=("Segoe UI", 10, "bold")).pack(anchor=tk.W, padx=5, pady=2)
        
        columns = ("name", "size_readable", "size_bytes", "kind", "pipeline", "fullpath")
        self.file_tree = ttk.Treeview(right_frame, columns=columns, show="headings", selectmode="extended")
        self.file_tree.configure(displaycolumns=("name", "size_readable", "size_bytes", "kind"))
        
        headers = {
            "name": "Name",
            "size_readable": "Size",
            "size_bytes": "Bytes",
            "kind": "Type",
            "pipeline": "Pipeline / Codec",
            "fullpath": "Full Path"
        }
        for col, txt in headers.items():
            self.file_tree.heading(col, text=txt, command=lambda c=col: self.exp_sort_column(c, False))
            self.file_tree.column(col, width=110, anchor=tk.W)
        self.file_tree.column("name", width=210)
        self.file_tree.column("size_bytes", anchor=tk.E)
        self.file_tree.column("fullpath", width=220)
        
        self.file_tree.bind("<<TreeviewSelect>>", self.on_file_select)
        
        r_scrollbar = ttk.Scrollbar(right_frame, orient=tk.VERTICAL, command=self.file_tree.yview)
        self.file_tree.configure(yscrollcommand=r_scrollbar.set)
        self.file_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        r_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Context menu
        self.exp_menu = tk.Menu(self.root, tearoff=0, bg=self.bg_card, fg=self.fg_light, activebackground=self.accent, activeforeground="#ffffff")
        self.exp_menu.add_command(label="Extract Selected...", command=self.exp_extract_selected)
        self.exp_menu.add_command(label="Extract Here", command=self.exp_extract_here)
        self.exp_menu.add_command(label="Test Archive", command=self.exp_verify)
        self.exp_menu.add_separator()
        self.exp_menu.add_command(label="Copy Path", command=self.exp_copy_path)
        self.exp_menu.add_command(label="Properties...", command=self.show_file_properties)
        self.file_tree.bind("<Button-3>", self.exp_show_context_menu)
        self.file_tree.bind("<Double-1>", self.exp_on_double_click)
        self.file_tree.bind("<Return>", self.exp_on_double_click)
        self._suppress_folder_event = False
        self.update_nav_buttons()

    def update_nav_buttons(self):
        if hasattr(self, "btn_back"):
            self.btn_back.config(state=tk.NORMAL if self.nav_index > 0 else tk.DISABLED)
        if hasattr(self, "btn_forward"):
            self.btn_forward.config(state=tk.NORMAL if self.nav_index < len(self.nav_history) - 1 else tk.DISABLED)
        if hasattr(self, "btn_up"):
            self.btn_up.config(state=tk.NORMAL if self.current_folder_prefix else tk.DISABLED)

    def _archive_counts(self):
        files = 0
        folders = set()
        for entry in self.all_archive_entries:
            p = entry["path"]
            kind = entry.get("kind", "FILE")
            if kind == "DIR":
                folders.add(p.rstrip("/"))
            else:
                files += 1
            parts = p.split("/")
            if len(parts) > 1:
                curr = ""
                for part in parts[:-1]:
                    curr = f"{curr}/{part}" if curr else part
                    folders.add(curr)
        return files, len(folders)

    def update_explorer_status(self):
        if not self.loaded_archive_path:
            self.update_status(mid="No archive loaded")
            return
        file_count, folder_count = self._archive_counts()
        sel_items = self.file_tree.selection()
        sel_count = len(sel_items)
        sel_bytes = 0
        for s in sel_items:
            vals = self.file_tree.item(s, "values")
            if len(vals) >= 3:
                raw = str(vals[2]).replace(",", "").strip()
                if raw.isdigit():
                    sel_bytes += int(raw)
        arch_sz = os.path.getsize(self.loaded_archive_path) if os.path.exists(self.loaded_archive_path) else 0
        orig_sz = int(self.archive_meta.get("orig_size", 0))
        ratio = (arch_sz / orig_sz * 100.0) if orig_sz > 0 else 0.0
        sel_part = f"Selected: {sel_count} ({self.format_size(sel_bytes)})" if sel_count > 0 else "Selected: 0"
        f_word = "file" if file_count == 1 else "files"
        d_word = "folder" if folder_count == 1 else "folders"
        name = os.path.basename(self.loaded_archive_path)
        self.update_status(
            mid=f"Archive: {name} | {file_count} {f_word}, {folder_count} {d_word} | {sel_part} | Ratio: {ratio:.2f}%"
        )

    def on_file_select(self, event=None):
        if self.loaded_archive_path and self.notebook.select() == str(self.tab_explorer):
            self.update_explorer_status()

    def exp_open_archive(self):
        f = filedialog.askopenfilename(title="Open PRISM Archive", filetypes=[("PRISM Archives", "*.prism"), ("All Files", "*.*")])
        if not f:
            return
        self.loaded_archive_path = f
        self.add_recent_archive(f)
        self.load_archive_details()

    def close_archive(self):
        self.loaded_archive_path = None
        self.all_archive_entries = []
        self.archive_meta = {}
        self.archive_pmet = None
        self.archive_active_pipes = []
        self.current_folder_prefix = ""
        self.nav_history = [""]
        self.nav_index = 0
        self.exp_path_var.set("Archive:\\")
        for item in self.folder_tree.get_children():
            self.folder_tree.delete(item)
        for item in self.file_tree.get_children():
            self.file_tree.delete(item)
        self.exp_verify_pill.config(text="UNCHECKED", bg="#555555")
        for w in self.exp_badges_container.winfo_children():
            w.destroy()
        self.update_status(left="Closed Archive", mid="No archive loaded")
        self.log_activity("Closed archive.")
        self.explorer_content_frame.pack_forget()
        self.empty_state_frame.pack(fill=tk.BOTH, expand=True)
        self.update_toolbar_states()
        self.update_nav_buttons()

    def load_archive_details(self):
        if not self.loaded_archive_path:
            return
        archive_path = self.loaded_archive_path
        self.empty_state_frame.pack_forget()
        self.explorer_content_frame.pack(fill=tk.BOTH, expand=True)
        self.cancel_requested = False
        self.start_busy("Loading archive...")
        self.exp_verify_pill.config(text="LOADING...", bg="#6b7280")
        self.log_activity(f"Loading archive: {archive_path}")

        def worker(target_path=archive_path):
            try:
                cmd = [prism.find_prism_exe(), "l", target_path]
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=prism._NO_WINDOW)
                self.running_process = proc
                stdout, stderr = proc.communicate()
                self.running_process = None
                if self.cancel_requested:
                    self.ui_queue.put(lambda: self.log_activity("Archive loading cancelled by user."))
                    self.ui_queue.put(self.stop_busy)
                    return
                if proc.returncode != 0:
                    err_msg = (stderr or stdout or "Failed to list archive entries.").strip()
                    def _err():
                        self.stop_busy()
                        if self.loaded_archive_path == target_path:
                            self.exp_verify_pill.config(text="ERROR", bg="#c62828")
                            messagebox.showerror("Error", f"Failed to load archive: {err_msg}")
                            self.log_activity(f"Error loading archive: {err_msg}")
                    self.ui_queue.put(_err)
                    return

                meta, entries = prism.parse_list_output(stdout)
                pmet_info = None
                try:
                    pmet_info = prism.read_metadata(target_path)
                except Exception:
                    pmet_info = None
                try:
                    flags_val = int(meta.get("flags", 0) or 0)
                except ValueError:
                    flags_val = 0
                if not flags_val and pmet_info:
                    try:
                        flags_val = int(pmet_info.get("flags", 0) or 0)
                    except (TypeError, ValueError):
                        flags_val = 0
                flag_map = [
                    (1 << 0, "CDC"),
                    (1 << 1, "Tar-Demux"),
                    (1 << 2, "Stride"),
                    (1 << 3, "x86 BCJ2"),
                    (1 << 4, "SPARC"),
                    (1 << 5, "ZIP-Precomp"),
                    (1 << 7, "LZMA"),
                    (1 << 8, "LZMA2"),
                ]
                active_pipes = [label for bit, label in flag_map if flags_val & bit]

                def _apply():
                    self.stop_busy()
                    if self.loaded_archive_path != target_path:
                        return
                    self.archive_meta = meta
                    self.all_archive_entries = entries
                    self.archive_active_pipes = active_pipes
                    self.archive_pmet = pmet_info

                    has_per_file_pipeline = any(
                        e.get("pipeline") or e.get("codec") or e.get("bucket")
                        for e in self.all_archive_entries
                    )
                    if has_per_file_pipeline:
                        self.file_tree.configure(displaycolumns=("name", "size_readable", "size_bytes", "kind", "pipeline"))
                    else:
                        self.file_tree.configure(displaycolumns=("name", "size_readable", "size_bytes", "kind"))

                    self._render_explorer_badges()

                    self.exp_verify_pill.config(text="READY (Test to verify)", bg="#3b82f6")
                    self.current_folder_prefix = ""
                    self.nav_history = [""]
                    self.nav_index = 0
                    self.exp_path_var.set("Archive:\\")
                    self.populate_folder_tree()
                    self.populate_file_tree(self.current_folder_prefix)
                    self.update_status(left="Archive Loaded")
                    self.update_explorer_status()
                    self.update_toolbar_states()
                    self.update_nav_buttons()
                self.ui_queue.put(_apply)
            except Exception as e:
                err_msg = str(e)
                def _exc():
                    self.stop_busy()
                    messagebox.showerror("Error", f"Failed to load archive: {err_msg}")
                    self.log_activity(f"Error loading archive: {err_msg}")
                self.ui_queue.put(_exc)
            finally:
                self.running_process = None

        threading.Thread(target=worker, daemon=True).start()

    def _render_explorer_badges(self):
        for w in self.exp_badges_container.winfo_children():
            w.destroy()
        active_pipes = getattr(self, "archive_active_pipes", []) or []
        badges_to_show = active_pipes if active_pipes else ["Standard"]
        for pipe in badges_to_show:
            tk.Label(
                self.exp_badges_container,
                text=pipe,
                font=("Segoe UI", 8, "bold"),
                bg="#005a9e",
                fg="#ffffff",
                padx=7,
                pady=2
            ).pack(side=tk.LEFT, padx=3)
        pmet_info = getattr(self, "archive_pmet", None)
        if pmet_info and pmet_info.get("has_pmet") and isinstance(pmet_info.get("pmet"), dict):
            pm = pmet_info["pmet"]
            preset_lbl = pm.get("preset") or "PRISM"
            orig_b = pm.get("orig_bytes") or pmet_info.get("orig_bytes") or 0
            enc_ms = pm.get("enc_ms")
            if enc_ms is not None and float(enc_ms) > 0 and orig_b > 0:
                spd_str = self.format_speed_from_bytes(orig_b, float(enc_ms) / 1000.0)
                badge_txt = f"{preset_lbl} · {spd_str}"
            else:
                badge_txt = f"{preset_lbl}"
            tk.Label(
                self.exp_badges_container,
                text=badge_txt,
                font=("Segoe UI", 8),
                bg="#2f4f4f",
                fg="#d1fae5",
                padx=7,
                pady=2
            ).pack(side=tk.LEFT, padx=3)

    def populate_folder_tree(self):
        for item in self.folder_tree.get_children():
            self.folder_tree.delete(item)
            
        root_node = self.folder_tree.insert("", tk.END, text="📁 Root (\\)", values=("",))
        
        folders = set()
        for entry in self.all_archive_entries:
            p = entry["path"]
            parts = p.split("/")
            if len(parts) > 1:
                curr = ""
                for part in parts[:-1]:
                    parent = curr
                    curr = f"{curr}/{part}" if curr else part
                    folders.add((parent, curr, part))
                    
        self._folder_node_map = {"": root_node}
        for parent, curr, part in sorted(folders, key=lambda x: x[1]):
            par_node = self._folder_node_map.get(parent, root_node)
            node = self.folder_tree.insert(par_node, tk.END, text=f"📁 {part}", values=(curr,))
            self._folder_node_map[curr] = node
            
        self.folder_tree.item(root_node, open=True)
        if self.folder_tree.selection() != (root_node,):
            self.folder_tree.selection_set(root_node)

    def _navigate_to_folder(self, prefix, push_history=True):
        prefix = prefix.strip("/")
        self.current_folder_prefix = prefix
        win_prefix = prefix.replace("/", "\\")
        self.exp_path_var.set(f"Archive:\\{win_prefix}")
        if push_history:
            if self.nav_index < len(self.nav_history) - 1:
                self.nav_history = self.nav_history[: self.nav_index + 1]
            if not self.nav_history or self.nav_history[-1] != prefix:
                self.nav_history.append(prefix)
                self.nav_index = len(self.nav_history) - 1
        if hasattr(self, "_folder_node_map") and prefix in self._folder_node_map:
            node = self._folder_node_map[prefix]
            self.folder_tree.see(node)
            if self.folder_tree.selection() != (node,):
                self.folder_tree.selection_set(node)
        self.populate_file_tree(prefix)
        self.update_nav_buttons()

    def on_folder_select(self, event):
        selected = self.folder_tree.selection()
        if not selected:
            return
        vals = self.folder_tree.item(selected[0], "values")
        if vals is not None and len(vals) > 0:
            prefix = vals[0].strip("/")
            if prefix != self.current_folder_prefix:
                self._navigate_to_folder(prefix, push_history=True)

    def exp_go_back(self):
        if self.nav_index > 0:
            self.nav_index -= 1
            self._navigate_to_folder(self.nav_history[self.nav_index], push_history=False)

    def exp_go_forward(self):
        if self.nav_index < len(self.nav_history) - 1:
            self.nav_index += 1
            self._navigate_to_folder(self.nav_history[self.nav_index], push_history=False)

    def exp_go_up(self):
        if not self.current_folder_prefix:
            return
        parts = self.current_folder_prefix.split("/")
        parent_prefix = "/".join(parts[:-1]) if len(parts) > 1 else ""
        self._navigate_to_folder(parent_prefix, push_history=True)

    def format_size(self, size_bytes):
        k_div, m_div, g_div, k_lbl, m_lbl, g_lbl, _ = self._get_unit_spec()
        val = float(size_bytes or 0)
        if abs(val) < k_div:
            return f"{int(val)} B" if float(val).is_integer() else f"{val:.0f} B"
        elif abs(val) < m_div:
            return f"{val / k_div:.2f} {k_lbl}"
        elif abs(val) < g_div:
            return f"{val / m_div:.2f} {m_lbl}"
        else:
            return f"{val / g_div:.2f} {g_lbl}"

    def populate_file_tree(self, folder_prefix=""):
        for item in self.file_tree.get_children():
            self.file_tree.delete(item)
            
        query = self.exp_search_var.get().strip().lower()
        match_count = 0

        if query:
            for entry in self.all_archive_entries:
                path = entry["path"]
                if query not in path.lower():
                    continue
                self._insert_file_row(entry)
                match_count += 1
            self.match_count_lbl.config(text=f"({match_count} matches)")
        else:
            prefix_slash = (folder_prefix + "/") if folder_prefix else ""
            child_folders = {}
            direct_files = []

            for entry in self.all_archive_entries:
                path = entry["path"]
                if prefix_slash and not path.startswith(prefix_slash):
                    continue
                rel = path[len(prefix_slash):] if prefix_slash else path
                if not rel:
                    continue
                if "/" in rel:
                    sub_name = rel.split("/", 1)[0]
                    sub_full = (prefix_slash + sub_name).strip("/")
                    child_folders[sub_full] = child_folders.get(sub_full, 0) + entry.get("size", 0)
                else:
                    if entry.get("kind") == "DIR":
                        sub_full = path.rstrip("/")
                        child_folders.setdefault(sub_full, 0)
                    else:
                        direct_files.append(entry)

            for sub_full in sorted(child_folders.keys()):
                sub_name = sub_full.rsplit("/", 1)[-1]
                sz_bytes = child_folders[sub_full]
                sz_readable = self.format_size(sz_bytes)
                self.file_tree.insert(
                    "", tk.END,
                    values=(f"📁 {sub_name}", sz_readable, f"{sz_bytes:,}", "Folder", "", sub_full)
                )
            for entry in sorted(direct_files, key=lambda e: os.path.basename(e["path"]).lower()):
                self._insert_file_row(entry)

            self.match_count_lbl.config(text="")

        self.update_explorer_status()

    def _insert_file_row(self, entry):
        path = entry["path"]
        name = os.path.basename(path)
        sz_bytes = entry["size"]
        sz_readable = self.format_size(sz_bytes)
        kind = entry["kind"]

        ext = Path(name).suffix.lower()
        icon = "📄"
        if ext in [".png", ".jpg", ".jpeg", ".gif", ".bmp"]:
            icon = "🖼"
        elif ext in [".py", ".js", ".cpp", ".h", ".json", ".xml", ".html", ".css"]:
            icon = "📜"
        elif ext in [".zip", ".prism", ".tar", ".gz", ".7z", ".rar"]:
            icon = "📦"
        elif ext in [".mp3", ".wav", ".flac"]:
            icon = "🎵"
        elif ext in [".mp4", ".mkv", ".avi"]:
            icon = "🎬"

        display_name = f"{icon} {name}"
        pipeline_info = entry.get("pipeline") or entry.get("codec") or entry.get("bucket") or ""
        self.file_tree.insert("", tk.END, values=(display_name, sz_readable, f"{sz_bytes:,}", kind, pipeline_info, path))

    def exp_filter_tree(self, *args):
        self.populate_file_tree(self.current_folder_prefix)

    def exp_sort_column(self, col, reverse):
        l = [(self.file_tree.set(k, col), k) for k in self.file_tree.get_children('')]
        if col == "size_bytes":
            l.sort(key=lambda t: int(t[0].replace(",", "")) if t[0].replace(",", "").isdigit() else 0, reverse=reverse)
        else:
            l.sort(reverse=reverse)
        for index, (val, k) in enumerate(l):
            self.file_tree.move(k, '', index)
        self.file_tree.heading(col, command=lambda: self.exp_sort_column(col, not reverse))

    def exp_show_context_menu(self, event):
        item = self.file_tree.identify_row(event.y)
        if item:
            self.file_tree.selection_set(item)
            self.exp_menu.post(event.x_root, event.y_root)

    def exp_on_double_click(self, event=None):
        selected = self.file_tree.selection()
        if selected:
            vals = self.file_tree.item(selected[0], "values")
            kind = vals[3]
            path = vals[5]
            if kind == "Folder":
                self._navigate_to_folder(path, push_history=True)
            else:
                self.extract_and_open_temp(path)

    def extract_and_open_temp(self, entry_path):
        if not self.loaded_archive_path:
            return
        archive_path = self.loaded_archive_path
        try:
            st = os.stat(archive_path)
            cache_key = (archive_path, st.st_mtime_ns, st.st_size, entry_path)
        except OSError:
            cache_key = None

        if cache_key and cache_key in self._preview_cache:
            cached_file = self._preview_cache[cache_key]
            if os.path.exists(cached_file):
                try:
                    os.startfile(cached_file)
                    self.log_activity(f"Opened cached preview: {cached_file}")
                    return
                except Exception as e:
                    messagebox.showerror("Error", f"Could not open file: {e}")
                    return

        self.cancel_requested = False
        self.start_busy(f"Opening {os.path.basename(entry_path)}...")

        def worker(target_arc=archive_path, ep=entry_path, ckey=cache_key):
            try:
                tmp = tempfile.mkdtemp(prefix="prism_view_")
                out_file = os.path.join(tmp, os.path.basename(ep))
                cmd = [prism.find_prism_exe(), "x1", target_arc, ep, out_file]
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=prism._NO_WINDOW)
                self.running_process = proc
                stdout, stderr = proc.communicate()
                self.running_process = None
                if self.cancel_requested:
                    self.ui_queue.put(lambda: self.log_activity("Preview extraction cancelled."))
                    return
                if proc.returncode == 0 and os.path.exists(out_file):
                    def _open():
                        if ckey:
                            self._preview_cache[ckey] = out_file
                        try:
                            os.startfile(out_file)
                            self.log_activity(f"Opened file in temp view: {out_file}")
                            self.update_status(left="Ready")
                        except Exception as e:
                            messagebox.showerror("Error", f"Could not open file: {e}")
                    self.ui_queue.put(_open)
                else:
                    err_msg = stderr or stdout or "Extraction failed."
                    self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", f"Failed to extract file for viewing: {m}"))
            except Exception as e:
                err_msg = str(e)
                self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", f"Could not open file: {m}"))
            finally:
                self.running_process = None
                self.ui_queue.put(self.stop_busy)

        threading.Thread(target=worker, daemon=True).start()

    def exp_copy_path(self):
        selected = self.file_tree.selection()
        if selected:
            paths = [self.file_tree.item(s, "values")[5] for s in selected]
            self.root.clipboard_clear()
            self.root.clipboard_append("\n".join(paths))
            self.log_activity(f"Copied {len(paths)} path(s) to clipboard.")

    def exp_verify(self):
        if not self.loaded_archive_path:
            messagebox.showerror("Error", "No archive loaded.")
            return
        archive_path = self.loaded_archive_path
        self.cancel_requested = False
        self.start_busy("Verifying archive...")
        self.exp_verify_pill.config(text="VERIFYING...", bg="#6b7280")
        self.log_activity(f"Verifying archive integrity: {archive_path}")

        def worker(target_path=archive_path):
            try:
                cmd = [prism.find_prism_exe(), "t", target_path]
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=prism._NO_WINDOW)
                self.running_process = proc
                stdout, stderr = proc.communicate()
                self.running_process = None
                if self.cancel_requested:
                    def _cancel():
                        if self.loaded_archive_path == target_path:
                            self.exp_verify_pill.config(text="READY (Test to verify)", bg="#3b82f6")
                        self.log_activity("Archive verification cancelled.")
                    self.ui_queue.put(_cancel)
                    return

                passed = (proc.returncode == 0)
                err_msg = stderr or stdout or "Verification failed."
                def _apply():
                    if self.loaded_archive_path != target_path:
                        return
                    if passed:
                        self.exp_verify_pill.config(text="VERIFIED OK", bg="#2e7d32")
                        self.update_status(left="Verify Passed")
                        self.log_activity("Archive verification PASSED.")
                        messagebox.showinfo("Verification Passed", "Archive integrity verified successfully (`prism.exe t`).")
                    else:
                        self.exp_verify_pill.config(text="CORRUPT", bg="#c62828")
                        self.update_status(left="Verify Failed")
                        self.log_activity("Archive verification FAILED.")
                        messagebox.showerror("Verification Failed", err_msg)
                self.ui_queue.put(_apply)
            except Exception as e:
                err_msg = str(e)
                self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Verification Error", m))
            finally:
                self.running_process = None
                self.ui_queue.put(self.stop_busy)

        threading.Thread(target=worker, daemon=True).start()

    def show_archive_info_dialog(self):
        if not self.loaded_archive_path:
            messagebox.showerror("Error", "No archive loaded.")
            return
        info_str = prism.info(self.loaded_archive_path)
        top = tk.Toplevel(self.root)
        top.title("Archive Information")
        top.geometry("520x360")
        top.configure(bg=self.bg_dark)
        st = scrolledtext.ScrolledText(top, bg=self.log_bg, fg=self.fg_light, font=("Consolas", 10))
        st.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        st.insert(tk.END, info_str)
        st.config(state=tk.DISABLED)

    def show_file_properties(self):
        selected = self.file_tree.selection()
        if not selected:
            return
        item_vals = self.file_tree.item(selected[0], "values")
        size_readable = item_vals[1]
        kind = item_vals[3]
        pipeline = item_vals[4]
        path = item_vals[5]
        
        top = tk.Toplevel(self.root)
        top.title(f"Properties: {os.path.basename(path)}")
        top.geometry("420x280")
        top.configure(bg=self.bg_dark)
        
        f = ttk.Frame(top, padding=15)
        f.pack(fill=tk.BOTH, expand=True)
        
        ttk.Label(f, text=f"Name: {os.path.basename(path) or path}", font=("Segoe UI", 10, "bold")).pack(anchor=tk.W, pady=4)
        ttk.Label(f, text=f"Full Path: {path}").pack(anchor=tk.W, pady=4)
        ttk.Label(f, text=f"Size: {size_readable} ({item_vals[2]} bytes)").pack(anchor=tk.W, pady=4)
        ttk.Label(f, text=f"Kind: {kind}").pack(anchor=tk.W, pady=4)
        if pipeline:
            ttk.Label(f, text=f"Pipeline / Codec: {pipeline}").pack(anchor=tk.W, pady=4)
        elif getattr(self, "archive_active_pipes", None):
            ttk.Label(f, text=f"Archive Pipelines: {', '.join(self.archive_active_pipes)}").pack(anchor=tk.W, pady=4)

    @staticmethod
    def _norm_preset(name):
        s = str(name)
        if s == "Fast" or s.startswith("Ultra (Max)") or s.startswith("Level "):
            return s
        return "Normal - Default"

    def show_settings_dialog(self):
        top = tk.Toplevel(self.root)
        top.title("PRISM Settings")
        top.geometry("460x430")
        top.configure(bg=self.bg_dark)

        f = ttk.Frame(top, padding=15)
        f.pack(fill=tk.BOTH, expand=True)

        ttk.Label(f, text="Default Preset:", font=("Segoe UI", 10, "bold")).grid(row=0, column=0, sticky=tk.W, pady=8)
        preset_var = tk.StringVar(value=self._norm_preset(self.settings.get("default_preset", "Normal - Default")))
        ttk.Combobox(f, textvariable=preset_var, values=["Normal - Default", "Ultra (Max)", "Level 7 - Very High", "Level 3 - Balanced", "Fast"], state="readonly", width=22).grid(row=0, column=1, sticky=tk.W, pady=8)

        ttk.Label(f, text="Default Threads:", font=("Segoe UI", 10, "bold")).grid(row=1, column=0, sticky=tk.W, pady=8)
        threads_var = tk.StringVar(value=self.settings.get("default_threads", "Auto"))
        ttk.Combobox(f, textvariable=threads_var, values=["Auto", "1", "2", "4", "8", "12"], state="readonly", width=10).grid(row=1, column=1, sticky=tk.W, pady=8)

        ttk.Label(f, text="Size / Speed Units:", font=("Segoe UI", 10, "bold")).grid(row=2, column=0, sticky=tk.W, pady=8)
        unit_var = tk.StringVar(value=self.settings.get("size_unit", "MB (1024)"))
        ttk.Combobox(f, textvariable=unit_var, values=["MB (1024)", "MB (SI 1000)", "MiB (IEC 1024)"], state="readonly", width=20).grid(row=2, column=1, sticky=tk.W, pady=8)

        verify_var = tk.BooleanVar(value=self.settings.get("auto_verify", True))
        ttk.Checkbutton(f, text="Auto-verify archives after compression", variable=verify_var).grid(row=3, column=0, columnspan=2, sticky=tk.W, pady=8)

        embed_meta_var = tk.BooleanVar(value=self.settings.get("embed_meta", True))
        ttk.Checkbutton(f, text="Embed compression stats metadata by default", variable=embed_meta_var).grid(row=4, column=0, columnspan=2, sticky=tk.W, pady=8)

        confirm_var = tk.BooleanVar(value=self.settings.get("confirm_overwrite", True))
        ttk.Checkbutton(f, text="Confirm on file overwrite", variable=confirm_var).grid(row=5, column=0, columnspan=2, sticky=tk.W, pady=8)

        log_var = tk.BooleanVar(value=self.settings.get("show_log_on_start", False))
        ttk.Checkbutton(f, text="Show activity log on start", variable=log_var).grid(row=6, column=0, columnspan=2, sticky=tk.W, pady=8)

        def save():
            self.settings["default_preset"] = preset_var.get()
            self.settings["default_threads"] = threads_var.get()
            self.settings["size_unit"] = unit_var.get()
            self.settings["auto_verify"] = verify_var.get()
            self.settings["embed_meta"] = embed_meta_var.get()
            self.settings["confirm_overwrite"] = confirm_var.get()
            self.settings["show_log_on_start"] = log_var.get()
            if hasattr(self, "comp_embed_meta_var"):
                self.comp_embed_meta_var.set(self.settings["embed_meta"])
            self.save_settings()
            self._refresh_unit_displays()
            top.destroy()
            messagebox.showinfo("Settings Saved", "Preferences successfully updated.")

        ttk.Button(f, text="Save Settings", style="Accent.TButton", command=save).grid(row=7, column=0, columnspan=2, pady=20)

    def show_about_dialog(self):
        messagebox.showinfo("About PRISM Archiver", "PRISM Archiver Studio v2.0\nProfessional Polymorphic Lossless Compressor & Archive Manager.\nRivals & beats WinRAR / 7-Zip.")

    def _confirm_overwrite_paths(self, target_paths, action_label="overwrite"):
        """Check if any target paths already exist; prompt user if confirm_overwrite is enabled."""
        if not self.settings.get("confirm_overwrite", True):
            return True
        existing = []
        seen = set()
        for p in target_paths:
            if not p:
                continue
            norm = os.path.normpath(p)
            if norm in seen:
                continue
            seen.add(norm)
            if os.path.exists(norm):
                existing.append(norm)
        if not existing:
            return True

        preview_lines = [f"  • {os.path.basename(p) or p}" for p in existing[:5]]
        if len(existing) > 5:
            preview_lines.append(f"  ... and {len(existing) - 5} more")
        details = "\n".join(preview_lines)
        msg = (
            f"{len(existing)} existing file(s) will be overwritten:\n\n"
            f"{details}\n\n"
            f"Do you want to continue and {action_label}?"
        )
        return bool(messagebox.askyesno("Confirm Overwrite", msg))

    def _extract_archive_to_dir(self, target):
        archive_path = self.loaded_archive_path
        entries = self.all_archive_entries
        is_dir_arc = str(self.archive_meta.get("is_dir", "0")) == "1"
        if not entries and archive_path:
            try:
                meta, entries = prism.list_entries(archive_path)
                is_dir_arc = str(meta.get("is_dir", "0")) == "1"
            except Exception:
                entries = []

        target_paths = []
        if is_dir_arc or os.path.isdir(target):
            for e in entries:
                if e.get("kind") != "DIR":
                    rel = e["path"].replace("/", os.sep)
                    target_paths.append(os.path.join(target, rel))
        else:
            target_paths.append(target)

        if not self._confirm_overwrite_paths(target_paths, action_label="overwrite the existing file(s)"):
            self.log_activity("Extraction cancelled (overwrite declined).")
            return

        orig_bytes = int(self.archive_meta.get("orig_size", 0))
        self.cancel_requested = False
        self.log_activity(f"Extracting archive to '{target}'...")
        self.start_busy("Extracting archive...")
        start_t = time.perf_counter()

        def worker():
            try:
                cmd_args = [prism.find_prism_exe(), "d", archive_path, target]
                proc = subprocess.Popen(cmd_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=prism._NO_WINDOW)
                self.running_process = proc
                stdout, stderr = proc.communicate()
                self.running_process = None
                elapsed = time.perf_counter() - start_t
                if self.cancel_requested:
                    self.ui_queue.put(lambda: self.log_activity("Extraction cancelled by user."))
                    return
                if proc.returncode == 0:
                    speed_str = self.format_speed_from_bytes(orig_bytes, elapsed)
                    self.ui_queue.put(lambda: self.status_timer_lbl.config(text=f"{elapsed:.2f}s"))
                    self.ui_queue.put(lambda s=speed_str: self.status_speed_lbl.config(text=s))
                    self.ui_queue.put(lambda: self.update_status(left="Extract Complete"))
                    self.ui_queue.put(lambda: self.log_activity(f"Extraction completed successfully in {elapsed:.2f}s."))
                    self.ui_queue.put(lambda: messagebox.showinfo("Success", f"Archive extracted successfully in {elapsed:.2f}s to:\n{target}"))
                else:
                    err_msg = stderr or stdout or "Extraction failed."
                    self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", m))
                    self.ui_queue.put(lambda: self.log_activity("Extraction failed."))
            except Exception as e:
                err_msg = str(e)
                self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", m))
            finally:
                self.running_process = None
                self.ui_queue.put(self.stop_busy)

        threading.Thread(target=worker, daemon=True).start()

    def exp_extract_all(self):
        if not self.loaded_archive_path:
            messagebox.showerror("Error", "No archive loaded.")
            return
        target = filedialog.askdirectory(title="Select Destination Directory")
        if not target:
            return
        self._extract_archive_to_dir(target)

    def exp_extract_here(self):
        if not self.loaded_archive_path:
            return
        target = os.path.dirname(self.loaded_archive_path)
        self._extract_archive_to_dir(target)

    def exp_extract_selected(self):
        if not self.loaded_archive_path:
            messagebox.showerror("Error", "No archive loaded.")
            return
        selected = self.file_tree.selection()
        if not selected:
            messagebox.showerror("Error", "No entries selected.")
            return
        d = filedialog.askdirectory(title="Select Destination Directory")
        if not d:
            return
        target_entries = []
        for s in selected:
            vals = self.file_tree.item(s, "values")
            kind = vals[3]
            entry_path = vals[5]
            if kind == "Folder":
                prefix = entry_path.rstrip("/") + "/"
                for e in self.all_archive_entries:
                    if e["path"].startswith(prefix) and e.get("kind") != "DIR":
                        target_entries.append(e["path"])
            else:
                target_entries.append(entry_path)

        unique_entries = list(dict.fromkeys(target_entries))
        if not unique_entries:
            messagebox.showinfo("Info", "No files found in selection.")
            return

        target_paths = [os.path.join(d, ep.replace("/", os.sep)) for ep in unique_entries]
        if not self._confirm_overwrite_paths(target_paths, action_label="overwrite the existing file(s)"):
            self.log_activity("Selected extraction cancelled (overwrite declined).")
            return

        archive_path = self.loaded_archive_path
        self.cancel_requested = False
        self.start_busy(f"Extracting {len(unique_entries)} item(s)...")
        self.log_activity(f"Extracting {len(unique_entries)} selected item(s) to '{d}'...")
        start_t = time.perf_counter()

        def worker(target_arc=archive_path, out_dir=d, entries=unique_entries):
            try:
                res = prism.extract_many(target_arc, out_dir, entries, check=False)
                elapsed = time.perf_counter() - start_t
                if self.cancel_requested:
                    self.ui_queue.put(lambda: self.log_activity("Selected extraction cancelled by user."))
                    return
                if res.returncode == 0:
                    def _ok():
                        self.status_timer_lbl.config(text=f"{elapsed:.2f}s")
                        self.update_status(left="Extract Selected Complete")
                        self.log_activity(f"Extracted {len(entries)} item(s) to '{out_dir}' in {elapsed:.2f}s.")
                        messagebox.showinfo("Success", f"All selected items extracted in {elapsed:.2f}s to:\n{out_dir}")
                    self.ui_queue.put(_ok)
                else:
                    err_msg = res.stderr or res.stdout or "Selected extraction failed."
                    self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", m))
                    self.ui_queue.put(lambda: self.log_activity("Selected extraction failed."))
            except Exception as e:
                err_msg = str(e)
                self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", m))
            finally:
                self.ui_queue.put(self.stop_busy)

        threading.Thread(target=worker, daemon=True).start()


    # ==================== TAB 2: PACK / COMPRESS QUEUE ====================
    def build_compress_tab(self):
        frame = self.tab_compress

        top_pane = ttk.Frame(frame)
        top_pane.pack(fill=tk.BOTH, expand=True, padx=6, pady=(6, 4))

        tb = ttk.Frame(top_pane)
        tb.pack(fill=tk.X, pady=(0, 4))

        ttk.Button(tb, text="➕ Add Files...", command=self.comp_add_files).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(tb, text="📁 Add Folder...", command=self.comp_add_folder).pack(side=tk.LEFT, padx=4)
        ttk.Button(tb, text="🗑 Remove Selected", command=self.comp_remove_selected).pack(side=tk.LEFT, padx=4)
        ttk.Button(tb, text="🧹 Clear Queue", command=self.comp_clear).pack(side=tk.LEFT, padx=4)

        _, _, m_lbl, _, _ = self._unit_labels()
        self.comp_summary_label = ttk.Label(tb, text=f"Queue Total: 0 items | 0.00 {m_lbl}", font=("Segoe UI", 9, "bold"))
        self.comp_summary_label.pack(side=tk.RIGHT, padx=4)

        tree_frame = ttk.Frame(top_pane)
        tree_frame.pack(fill=tk.BOTH, expand=True)

        columns = ("path", "type", "size")
        self.comp_tree = ttk.Treeview(tree_frame, columns=columns, show="headings", selectmode="extended")
        self.comp_tree.heading("path", text="Path")
        self.comp_tree.heading("type", text="Type")
        self.comp_tree.heading("size", text="Size (Bytes)")
        self.comp_tree.column("path", width=620, anchor=tk.W)
        self.comp_tree.column("type", width=90, anchor=tk.CENTER)
        self.comp_tree.column("size", width=140, anchor=tk.E)

        scrollbar = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.comp_tree.yview)
        self.comp_tree.configure(yscrollcommand=scrollbar.set)

        self.comp_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # Bottom Card: Compact WinRAR-style Parameters & Result Summary Card
        bottom_pane = ttk.Frame(frame, style="Card.TFrame")
        bottom_pane.pack(fill=tk.X, side=tk.BOTTOM, padx=6, pady=(2, 6))

        settings_grid = ttk.Frame(bottom_pane, style="Card.TFrame")
        settings_grid.pack(fill=tk.X, padx=10, pady=(8, 4))
        settings_grid.columnconfigure(1, weight=1)

        # Row 0: Archive output path
        ttk.Label(settings_grid, text="Archive name / path:", style="Card.TLabel", font=("Segoe UI", 9, "bold")).grid(row=0, column=0, sticky=tk.W, padx=(0, 8), pady=3)
        out_row = ttk.Frame(settings_grid, style="Card.TFrame")
        out_row.grid(row=0, column=1, columnspan=4, sticky=tk.EW, padx=(4, 0), pady=3)
        out_row.columnconfigure(0, weight=1)
        self.comp_out_var = tk.StringVar()
        out_entry = ttk.Entry(out_row, textvariable=self.comp_out_var)
        out_entry.grid(row=0, column=0, sticky=tk.EW, padx=(0, 6))
        ttk.Button(out_row, text="Browse...", command=self.comp_browse_output).grid(row=0, column=1, sticky=tk.E)

        # Row 1: Mode, Profile, Threads
        ttk.Label(settings_grid, text="Mode:", style="Card.TLabel").grid(row=1, column=0, sticky=tk.W, padx=(0, 8), pady=3)
        self.comp_mode_var = tk.StringVar(value="single")
        mode_frame = ttk.Frame(settings_grid, style="Card.TFrame")
        mode_frame.grid(row=1, column=1, sticky=tk.W, padx=4, pady=3)
        ttk.Radiobutton(mode_frame, text="Single .prism", variable=self.comp_mode_var, value="single", style="Card.TRadiobutton", command=self.auto_set_output_default).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Radiobutton(mode_frame, text="Batch (per item)", variable=self.comp_mode_var, value="batch", style="Card.TRadiobutton", command=self.auto_set_output_default).pack(side=tk.LEFT, padx=4)

        prof_frame = ttk.Frame(settings_grid, style="Card.TFrame")
        prof_frame.grid(row=1, column=2, columnspan=3, sticky=tk.E, pady=3)
        ttk.Label(prof_frame, text="Profile:", style="Card.TLabel").pack(side=tk.LEFT, padx=(8, 4))
        self.comp_preset_var = tk.StringVar(value=self._norm_preset(self.settings.get("default_preset", "Normal - Default")))
        ttk.Combobox(prof_frame, textvariable=self.comp_preset_var, values=["Normal - Default", "Ultra (Max)", "Level 7 - Very High", "Level 3 - Balanced", "Fast"], state="readonly", width=18).pack(side=tk.LEFT, padx=(0, 12))
        ttk.Label(prof_frame, text="Threads:", style="Card.TLabel").pack(side=tk.LEFT, padx=(0, 4))
        self.comp_threads_var = tk.StringVar(value=self.settings.get("default_threads", "Auto"))
        ttk.Combobox(prof_frame, textvariable=self.comp_threads_var, values=["Auto", "1", "2", "4", "8", "12"], state="readonly", width=7).pack(side=tk.LEFT)

        # Row 2: Options + Action Buttons
        options_frame = ttk.Frame(settings_grid, style="Card.TFrame")
        options_frame.grid(row=2, column=0, columnspan=3, sticky=tk.W, pady=4)

        self.comp_verify_var = tk.BooleanVar(value=self.settings.get("auto_verify", True))
        ttk.Checkbutton(options_frame, text="Verify after compression", variable=self.comp_verify_var, style="Card.TCheckbutton").grid(row=0, column=0, sticky=tk.W, padx=(0, 14), pady=1)

        self.comp_embed_meta_var = tk.BooleanVar(value=self.settings.get("embed_meta", True))
        ttk.Checkbutton(options_frame, text="Embed stats metadata", variable=self.comp_embed_meta_var, style="Card.TCheckbutton").grid(row=0, column=1, sticky=tk.W, padx=(0, 14), pady=1)

        self.comp_delete_source_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(options_frame, text="Delete source after success", variable=self.comp_delete_source_var, style="Card.TCheckbutton").grid(row=1, column=0, sticky=tk.W, padx=(0, 14), pady=1)

        self.comp_open_folder_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(options_frame, text="Open output folder when done", variable=self.comp_open_folder_var, style="Card.TCheckbutton").grid(row=1, column=1, sticky=tk.W, padx=(0, 14), pady=1)

        act_box = ttk.Frame(settings_grid, style="Card.TFrame")
        act_box.grid(row=2, column=3, columnspan=2, sticky=tk.E, pady=4)

        self.comp_btn = ttk.Button(act_box, text="🗜 Compress Now", style="Accent.TButton", command=self.start_compression)
        self.comp_btn.pack(side=tk.LEFT, padx=(0, 6))

        self.comp_open_explorer_btn = ttk.Button(act_box, text="📂 Open in Explorer", state=tk.DISABLED, command=self.comp_open_in_explorer_tab)
        self.comp_open_explorer_btn.pack(side=tk.LEFT, padx=(0, 6))

        self.comp_send_bench_btn = ttk.Button(act_box, text="📊 Send to Benchmark", state=tk.DISABLED, command=self.comp_send_to_benchmark)
        self.comp_send_bench_btn.pack(side=tk.LEFT)

        # Structured Result Summary Card
        summary_card = ttk.Frame(bottom_pane, style="CardBorder.TFrame", padding=(10, 6))
        summary_card.pack(fill=tk.X, padx=10, pady=(2, 8))

        self.comp_result_card = ttk.Label(summary_card, text="Last Pack Result: Ready — stage files and click Compress Now.", style="SubCardMuted.TLabel", font=("Segoe UI", 9, "bold"))
        self.comp_result_card.pack(anchor=tk.W, pady=(0, 4))

        metrics_row = ttk.Frame(summary_card, style="SubCard.TFrame")
        metrics_row.pack(fill=tk.X)
        self.comp_metric_labels = {}
        metric_defs = [
            ("orig", "Original"),
            ("packed", "Packed"),
            ("ratio", "Ratio"),
            ("time", "Time"),
            ("speed", "Speed"),
            ("verified", "Verified"),
        ]
        for idx, (key, title) in enumerate(metric_defs):
            metrics_row.columnconfigure(idx, weight=1)
            cell = ttk.Frame(metrics_row, style="SubCard.TFrame")
            cell.grid(row=0, column=idx, sticky=tk.W, padx=(0, 10))
            ttk.Label(cell, text=title, style="SubCardMuted.TLabel", font=("Segoe UI", 8)).pack(anchor=tk.W)
            val_lbl = ttk.Label(cell, text="—", style="SubCard.TLabel", font=("Consolas", 9, "bold"))
            val_lbl.pack(anchor=tk.W)
            self.comp_metric_labels[key] = val_lbl

        self.last_compressed_archive = None

    def _update_comp_result_card(self, header_text, orig="—", packed="—", ratio="—", elapsed="—", speed="—", verified="—"):
        self.comp_result_card.config(text=header_text, style="SubCard.TLabel")
        vals = {"orig": orig, "packed": packed, "ratio": ratio, "time": elapsed, "speed": speed, "verified": verified}
        for k, v in vals.items():
            if k in self.comp_metric_labels:
                self.comp_metric_labels[k].config(text=v)

    def comp_add_files(self):
        files = filedialog.askopenfilenames(title="Select Files to Compress")
        for f in files:
            if f:
                sz = os.path.getsize(f)
                self.comp_tree.insert("", tk.END, values=(f, "File", sz))
        self.update_comp_summary()
        self.auto_set_output_default()

    def comp_add_folder(self):
        d = filedialog.askdirectory(title="Select Folder to Compress")
        if d:
            total_size = sum(p.stat().st_size for p in Path(d).rglob("*") if p.is_file())
            self.comp_tree.insert("", tk.END, values=(d, "Folder", total_size))
            self.update_comp_summary()
            self.auto_set_output_default()

    def comp_remove_selected(self):
        selected = self.comp_tree.selection()
        for item in selected:
            self.comp_tree.delete(item)
        self.update_comp_summary()
        self.auto_set_output_default()

    def comp_clear(self):
        for item in self.comp_tree.get_children():
            self.comp_tree.delete(item)
        self._last_auto_out = ""
        self.comp_out_var.set("")
        self.update_comp_summary()

    def update_comp_summary(self):
        children = self.comp_tree.get_children()
        total_bytes = sum(int(self.comp_tree.item(c, "values")[2]) for c in children)
        m_div, _, m_lbl, _, _ = self._unit_labels()
        total_m = total_bytes / m_div
        self.comp_summary_label.config(text=f"Queue Total: {len(children)} items | {total_m:.2f} {m_lbl} ({total_bytes:,} bytes)")

    def auto_set_output_default(self):
        children = self.comp_tree.get_children()
        current_out = self.comp_out_var.get().strip()
        if current_out and current_out != self._last_auto_out:
            return
        if not children:
            self._last_auto_out = ""
            self.comp_out_var.set("")
            return
        mode = self.comp_mode_var.get()
        first_path = self.comp_tree.item(children[0], "values")[0]
        if mode == "batch":
            default_out = os.path.dirname(os.path.abspath(first_path))
        elif len(children) == 1:
            default_out = first_path + ".prism"
        else:
            parent_dir = os.path.dirname(os.path.abspath(first_path))
            default_out = os.path.join(parent_dir, "archive_bundle.prism")
        self._last_auto_out = default_out
        self.comp_out_var.set(default_out)

    def comp_browse_output(self):
        mode = self.comp_mode_var.get()
        if mode == "batch":
            d = filedialog.askdirectory(title="Select Output Directory for Batch Archives")
            if d:
                self.comp_out_var.set(d)
        else:
            f = filedialog.asksaveasfilename(defaultextension=".prism", filetypes=[("PRISM Archives", "*.prism"), ("All Files", "*.*")])
            if f:
                self.comp_out_var.set(f)

    @staticmethod
    def _parse_progress_line(line: str):
        """Parse `[PROGRESS] <pct> <stage> [<done> [<total>]]` lines from prism.exe stdout."""
        s = line.strip()
        if not s.startswith("[PROGRESS]"):
            return None
        rest = s[len("[PROGRESS]"):].strip()
        if not rest:
            return None
        parts = rest.split(None, 1)
        try:
            pct = float(parts[0].rstrip("%"))
        except ValueError:
            return None
        pct = max(0.0, min(100.0, pct))
        if len(parts) < 2:
            return pct, "Compressing...", None, None
        tokens = parts[1].strip().split()
        done_bytes = None
        total_bytes = None
        if len(tokens) >= 2 and tokens[-1].isascii() and tokens[-1].isdigit() and tokens[-2].isascii() and tokens[-2].isdigit():
            done_bytes = int(tokens[-2])
            total_bytes = int(tokens[-1])
            stage = " ".join(tokens[:-2]).strip() or "Compressing..."
        elif len(tokens) >= 1 and tokens[-1].isascii() and tokens[-1].isdigit():
            done_bytes = int(tokens[-1])
            stage = " ".join(tokens[:-1]).strip() or "Compressing..."
        else:
            stage = " ".join(tokens).strip() or "Compressing..."
        return pct, stage, done_bytes, total_bytes

    def _run_compress_process(self, cmd_args, env, live_state, item_orig_bytes, done_orig_bytes, total_orig_bytes, stage_prefix=""):
        """Run prism compress with line-buffered UTF-8 stdout streaming for live progress."""
        proc = subprocess.Popen(
            cmd_args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            env=env,
            creationflags=prism._NO_WINDOW,
        )
        self.running_process = proc

        stderr_lines = []

        def _drain_stderr():
            if proc.stderr is None:
                return
            try:
                for err_line in proc.stderr:
                    stderr_lines.append(err_line)
            except Exception:
                pass

        err_thread = threading.Thread(target=_drain_stderr, daemon=True)
        err_thread.start()

        stdout_lines = []
        if proc.stdout is not None:
            for line in proc.stdout:
                parsed = self._parse_progress_line(line)
                if parsed is not None:
                    item_pct, stage, done_bytes, total_bytes = parsed
                    now = time.perf_counter()
                    if total_orig_bytes > 0 and item_orig_bytes >= 0:
                        overall_pct = ((done_orig_bytes + (item_pct / 100.0) * item_orig_bytes) / total_orig_bytes) * 100.0
                    else:
                        overall_pct = item_pct
                    overall_pct = max(0.0, min(100.0, overall_pct))
                    live_state["pct"] = max(live_state["pct"] or 0.0, overall_pct)
                    live_state["stage"] = f"{stage_prefix}{stage}" if stage_prefix else stage

                    stage_l = stage.lower()
                    if stage_l.startswith("compressing"):
                        if done_bytes is not None and total_bytes and total_bytes > 0:
                            frac = min(1.0, max(0.0, done_bytes / total_bytes))
                            eff_bytes = done_orig_bytes + frac * item_orig_bytes
                            if eff_bytes > live_state["eff_bytes"]:
                                live_state["eff_bytes_time"] = now
                                live_state["eff_bytes"] = eff_bytes
                        elif done_bytes is not None:
                            eff_bytes = done_orig_bytes + (
                                min(float(item_orig_bytes), float(done_bytes)) if item_orig_bytes > 0 else float(done_bytes)
                            )
                            if eff_bytes > live_state["eff_bytes"]:
                                live_state["eff_bytes_time"] = now
                                live_state["eff_bytes"] = eff_bytes
                else:
                    stdout_lines.append(line)

        proc.wait()
        err_thread.join(timeout=1.0)
        for stream in (proc.stdout, proc.stderr):
            if stream is not None:
                try:
                    stream.close()
                except Exception:
                    pass
        self.running_process = None
        return proc.returncode, "".join(stdout_lines), "".join(stderr_lines)

    def start_compression(self):
        children = self.comp_tree.get_children()
        if not children:
            messagebox.showerror("Error", "No files or folders staged for compression.")
            return
        
        out_path = self.comp_out_var.get().strip()
        if not out_path:
            self.auto_set_output_default()
            out_path = self.comp_out_var.get().strip()
        if not out_path:
            messagebox.showerror("Error", "Please specify an output archive path or directory.")
            return
            
        items = [self.comp_tree.item(c, "values") for c in children]
        mode = self.comp_mode_var.get()
        preset = self.comp_preset_var.get()
        threads = self.comp_threads_var.get()
        verify_after = self.comp_verify_var.get()
        embed_meta = self.comp_embed_meta_var.get()
        delete_source = self.comp_delete_source_var.get()
        open_folder = self.comp_open_folder_var.get()

        if mode == "batch":
            target_archives = [os.path.join(out_path, Path(path_str).name + ".prism") for path_str, _, _ in items]
        else:
            target_archives = [out_path]

        if not self._confirm_overwrite_paths(target_archives, action_label="overwrite the existing archive(s)"):
            self.log_activity("Compression cancelled (overwrite declined).")
            return

        if delete_source:
            confirmed = messagebox.askyesno(
                "Confirm Delete Source",
                f"Are you sure you want to permanently delete {len(items)} source item(s) after compression succeeds?"
            )
            if not confirmed:
                return

        self.cancel_requested = False
        self.comp_btn.config(state=tk.DISABLED)
        self.comp_open_explorer_btn.config(state=tk.DISABLED)
        self.comp_send_bench_btn.config(state=tk.DISABLED)
        self.set_busy(True)

        total_orig_bytes = sum(int(it[2]) for it in items)
        live_state = {
            "active": True,
            "start_time": time.perf_counter(),
            "pct": None,
            "stage": "Starting...",
            "header": f"Compressing ({preset})...",
            "orig_str": self.format_size(total_orig_bytes),
            "eff_bytes": 0.0,
            "eff_bytes_time": None,
            "displayed_eff_bytes": 0.0,
            "last_speed_draw_time": 0.0,
            "displayed_speed_str": "Working...",
        }

        def tick_live_stats():
            if not self.is_busy or not live_state["active"]:
                return
            now = time.perf_counter()
            elapsed = now - live_state["start_time"]
            self.status_timer_lbl.config(text=f"{elapsed:.2f}s")
            pct = live_state["pct"]
            stage = live_state["stage"]
            eff = live_state["eff_bytes"]
            eff_t = live_state["eff_bytes_time"]
            if (eff > live_state["displayed_eff_bytes"] and eff_t is not None and
                (now - live_state["last_speed_draw_time"] >= 0.35 or (total_orig_bytes > 0 and eff >= total_orig_bytes))):
                dt_eff = eff_t - live_state["start_time"]
                if dt_eff >= 0.10:
                    live_state["displayed_speed_str"] = self.format_speed_from_bytes(eff, dt_eff)
                    live_state["displayed_eff_bytes"] = eff
                    live_state["last_speed_draw_time"] = now
            speed_str = live_state["displayed_speed_str"]
            if pct is not None:
                if str(self.status_progress.cget("mode")) != "determinate":
                    self.status_progress.stop()
                    self.status_progress.config(mode="determinate", maximum=100)
                self.status_progress.config(value=max(0.0, min(100.0, pct)))
                self.status_speed_lbl.config(text=speed_str)
                self.update_status(left=f"{stage} ({pct:.0f}%)")
                self._update_comp_result_card(
                    f"{live_state['header']} ({pct:.0f}%)",
                    orig=live_state["orig_str"],
                    packed="...",
                    ratio="...",
                    elapsed=f"{elapsed:.2f}s",
                    speed=speed_str,
                    verified="Pending" if verify_after else "Skipped",
                )
            else:
                self.status_speed_lbl.config(text=speed_str)
                self.update_status(left=stage)
                self._update_comp_result_card(
                    live_state["header"],
                    orig=live_state["orig_str"],
                    packed="...",
                    ratio="...",
                    elapsed=f"{elapsed:.2f}s",
                    speed=speed_str,
                    verified="Pending" if verify_after else "Skipped",
                )
            self.root.after(100, tick_live_stats)

        self.root.after(100, tick_live_stats)
        
        def worker():
            temp_dir_obj = None
            try:
                env = os.environ.copy()
                if threads != "Auto":
                    env["OMP_NUM_THREADS"] = threads
                    
                start_global = time.perf_counter()
                
                if mode == "batch":
                    os.makedirs(out_path, exist_ok=True)
                    success_count = 0
                    total_orig = 0
                    total_arch = 0
                    all_verified = True
                    done_orig = 0
                    
                    for idx, (path_str, kind, size_str) in enumerate(items, start=1):
                        if self.cancel_requested:
                            break
                        p = Path(path_str)
                        item_bytes = int(size_str)
                        target_out = os.path.join(out_path, p.name + ".prism")
                        live_state["stage"] = f"[{idx}/{len(items)}] Compressing {p.name}..."
                        self.ui_queue.put(lambda name=p.name, tout=target_out: self.log_activity(f"Compressing '{name}' -> '{tout}'..."))
                        
                        start_t = time.perf_counter()
                        cmd_args = [prism.find_prism_exe(), "c", str(p), target_out]
                        if preset == "Fast":
                            cmd_args.append("--fast")
                        elif str(preset).startswith("Ultra (Max)"):
                            cmd_args.append("--ultra")
                        elif str(preset).startswith("Level 7"):
                            cmd_args.extend(["--level", "7"])
                        elif str(preset).startswith("Level 3"):
                            cmd_args.extend(["--level", "3"])
                        if embed_meta:
                            cmd_args.append("--meta")
                        rc, stdout, stderr = self._run_compress_process(
                            cmd_args,
                            env,
                            live_state,
                            item_bytes,
                            done_orig,
                            total_orig_bytes,
                            stage_prefix=f"[{idx}/{len(items)}] ",
                        )
                        elapsed = time.perf_counter() - start_t
                        done_orig += item_bytes
                        if float(done_orig) > live_state["eff_bytes"]:
                            live_state["eff_bytes_time"] = time.perf_counter()
                            live_state["eff_bytes"] = float(done_orig)
                        
                        if self.cancel_requested:
                            if os.path.exists(target_out):
                                try:
                                    os.remove(target_out)
                                except Exception:
                                    pass
                            break

                        if rc == 0 and os.path.exists(target_out):
                            item_ok = True
                            if verify_after:
                                live_state["stage"] = f"[{idx}/{len(items)}] Verifying..."
                                v_res = prism.verify(target_out, check=False)
                                if v_res.returncode != 0:
                                    item_ok = False
                                    all_verified = False
                            success_count += 1
                            total_orig += item_bytes
                            total_arch += os.path.getsize(target_out)
                            self.ui_queue.put(lambda name=p.name, el=elapsed: self.log_activity(f"Batch item '{name}' compressed successfully in {el:.2f}s."))
                            
                            if delete_source and item_ok:
                                try:
                                    if p.is_file():
                                        p.unlink()
                                    elif p.is_dir():
                                        shutil.rmtree(p)
                                except Exception:
                                    pass

                    if self.cancel_requested:
                        self.ui_queue.put(lambda: self.log_activity("Batch compression cancelled by user."))
                        return
                            
                    elapsed_global = time.perf_counter() - start_global
                    ratio = (total_arch / total_orig * 100) if total_orig > 0 else 0
                    speed_str = self.format_speed_from_bytes(total_orig, elapsed_global)
                    v_str = "PASS" if (verify_after and all_verified) else ("FAIL" if verify_after else "Skipped")
                    
                    self.ui_queue.put(lambda: self.update_status(left="Batch Success", mid=f"Compressed {success_count} items | {self.format_size(total_arch)} ({ratio:.1f}%)"))
                    self.ui_queue.put(lambda: self.status_timer_lbl.config(text=f"{elapsed_global:.2f}s"))
                    self.ui_queue.put(lambda s=speed_str: self.status_speed_lbl.config(text=s))
                    self.ui_queue.put(lambda s=speed_str: self._update_comp_result_card(
                        f"Batch Result: {success_count}/{len(items)} archive(s) created in {out_path}",
                        orig=self.format_size(total_orig),
                        packed=self.format_size(total_arch),
                        ratio=f"{ratio:.2f}%",
                        elapsed=f"{elapsed_global:.2f}s",
                        speed=s,
                        verified=v_str,
                    ))
                    
                else:
                    if len(items) == 1:
                        src_target = items[0][0]
                    else:
                        live_state["stage"] = "Staging files..."
                        temp_dir_obj = tempfile.TemporaryDirectory()
                        staging_dir = Path(temp_dir_obj.name)
                        for path_str, kind, _ in items:
                            p = Path(path_str)
                            if p.is_file():
                                shutil.copy2(p, staging_dir / p.name)
                            elif p.is_dir():
                                shutil.copytree(p, staging_dir / p.name, dirs_exist_ok=True)
                        src_target = str(staging_dir)
                    
                    out_dir = os.path.dirname(os.path.abspath(out_path))
                    if out_dir:
                        os.makedirs(out_dir, exist_ok=True)
                    live_state["stage"] = "Compressing..."
                    self.ui_queue.put(lambda: self.log_activity(f"Starting compression: '{src_target}' -> '{out_path}' ({preset})"))
                    start_t = time.perf_counter()
                    
                    cmd_args = [prism.find_prism_exe(), "c", src_target, out_path]
                    if preset == "Fast":
                        cmd_args.append("--fast")
                    elif str(preset).startswith("Ultra (Max)"):
                        cmd_args.append("--ultra")
                    elif str(preset).startswith("Level 7"):
                        cmd_args.extend(["--level", "7"])
                    elif str(preset).startswith("Level 3"):
                        cmd_args.extend(["--level", "3"])
                    if embed_meta:
                        cmd_args.append("--meta")
                    rc, stdout, stderr = self._run_compress_process(
                        cmd_args,
                        env,
                        live_state,
                        total_orig_bytes,
                        0,
                        total_orig_bytes,
                    )
                    elapsed = time.perf_counter() - start_t

                    if self.cancel_requested:
                        if os.path.exists(out_path):
                            try:
                                os.remove(out_path)
                            except Exception:
                                pass
                        self.ui_queue.put(lambda: self.log_activity("Compression cancelled by user."))
                        return
                    
                    if rc != 0 or not os.path.exists(out_path):
                        err_text = (stderr or stdout or "Compression failed")
                        self.ui_queue.put(lambda m=err_text: messagebox.showerror("Compression Error", m))
                        self.ui_queue.put(lambda m=err_text: self.log_activity(f"Compression failed: {m}"))
                        return
                    
                    orig_bytes = total_orig_bytes
                    arch_bytes = os.path.getsize(out_path)
                    ratio = (arch_bytes / orig_bytes * 100) if orig_bytes > 0 else 0
                    speed_str = self.format_speed_from_bytes(orig_bytes, elapsed)
                    
                    self.ui_queue.put(lambda s=speed_str: self.log_activity(f"Compression completed successfully in {elapsed:.2f}s ({s}, Ratio: {ratio:.1f}%)."))
                    
                    verified_ok = True
                    if verify_after:
                        live_state["stage"] = "Verifying..."
                        self.ui_queue.put(lambda: self.log_activity(f"Verifying archive integrity: {out_path}"))
                        v_res = prism.verify(out_path, check=False)
                        if v_res.returncode == 0:
                            self.ui_queue.put(lambda: self.log_activity("Verification PASSED successfully."))
                        else:
                            verified_ok = False
                            self.ui_queue.put(lambda: self.log_activity("Verification FAILED."))
                    
                    if delete_source and verified_ok:
                        for path_str, _, _ in items:
                            try:
                                p = Path(path_str)
                                if p.is_file():
                                    p.unlink()
                                elif p.is_dir():
                                    shutil.rmtree(p)
                            except Exception:
                                pass
                    
                    status_mid = f"Success: {self.format_size(arch_bytes)} ({ratio:.1f}%)"
                    self.ui_queue.put(lambda: self.update_status(left="Pack Complete", mid=status_mid))
                    self.ui_queue.put(lambda: self.status_timer_lbl.config(text=f"{elapsed:.2f}s"))
                    self.ui_queue.put(lambda s=speed_str: self.status_speed_lbl.config(text=s))
                    
                    v_str = "PASS" if (verify_after and verified_ok) else ("FAIL" if verify_after else "Skipped")
                    self.ui_queue.put(lambda s=speed_str: self._update_comp_result_card(
                        f"Last Pack Result: {os.path.basename(out_path)}",
                        orig=self.format_size(orig_bytes),
                        packed=self.format_size(arch_bytes),
                        ratio=f"{ratio:.2f}%",
                        elapsed=f"{elapsed:.2f}s",
                        speed=s,
                        verified=v_str,
                    ))
                    
                    self.last_compressed_archive = out_path
                    self.ui_queue.put(lambda: self.comp_open_explorer_btn.config(state=tk.NORMAL))
                    self.ui_queue.put(lambda: self.comp_send_bench_btn.config(state=tk.NORMAL))
                    
                    if open_folder:
                        try:
                            os.startfile(os.path.dirname(os.path.abspath(out_path)))
                        except Exception:
                            pass
                
            except Exception as e:
                if not self.cancel_requested:
                    err_msg = str(e)
                    self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Error", m))
                    self.ui_queue.put(lambda m=err_msg: self.log_activity(f"Error during compression: {m}"))
            finally:
                live_state["active"] = False
                self.running_process = None
                if temp_dir_obj:
                    try:
                        temp_dir_obj.cleanup()
                    except Exception:
                        pass
                self.ui_queue.put(lambda: self.set_busy(False))
                self.ui_queue.put(lambda: self.comp_btn.config(state=tk.NORMAL))
                
        threading.Thread(target=worker, daemon=True).start()

    def comp_open_in_explorer_tab(self):
        if self.last_compressed_archive and os.path.exists(self.last_compressed_archive):
            self.notebook.select(self.tab_explorer)
            self.loaded_archive_path = self.last_compressed_archive
            self.add_recent_archive(self.last_compressed_archive)
            self.load_archive_details()

    def comp_send_to_benchmark(self):
        if self.last_compressed_archive and os.path.exists(self.last_compressed_archive):
            self.import_archives_to_benchmark([self.last_compressed_archive])
            self.notebook.select(self.tab_benchmark)
            self.bench_sub_notebook.select(self.tab_bench_custom)


    # ==================== TAB 3: BENCHMARK & VERSION DIFF ====================
    def build_benchmark_tab(self):
        frame = self.tab_benchmark

        self.bench_sub_notebook = ttk.Notebook(frame)
        self.bench_sub_notebook.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        # Sub-tab 0: Corpus Baselines (vs 7z)
        self.tab_bench_corpus = ttk.Frame(self.bench_sub_notebook)
        self.tab_bench_custom = ttk.Frame(self.bench_sub_notebook)
        self.bench_sub_notebook.add(self.tab_bench_corpus, text="📊 Corpus Baselines (vs 7z)")
        self.bench_sub_notebook.add(self.tab_bench_custom, text="🧪 Custom Benchmarks & Archive Compare")
        self.bench_sub_notebook.select(self.tab_bench_corpus)

        tb = ttk.Frame(self.tab_bench_corpus)
        tb.pack(fill=tk.X, padx=5, pady=5)

        ttk.Button(tb, text="🔄 Reload Benchmark JSON", command=self.load_benchmark_data).pack(side=tk.LEFT, padx=2)
        ttk.Button(tb, text="⚡ Quick Test Custom File/Folder", style="Accent.TButton", command=self.run_quick_test).pack(side=tk.LEFT, padx=10)

        ttk.Label(tb, text="Chart Metric:").pack(side=tk.LEFT, padx=(20, 5))
        metric_vals = self._get_corpus_metric_values()
        self.bench_metric_var = tk.StringVar(value=metric_vals[0])
        self.bench_metric_combo = ttk.Combobox(tb, textvariable=self.bench_metric_var, values=metric_vals, state="readonly", width=24)
        self.bench_metric_combo.pack(side=tk.LEFT, padx=2)
        self.bench_metric_combo.bind("<<ComboboxSelected>>", lambda e: self.draw_benchmark_chart())

        paned = ttk.PanedWindow(self.tab_bench_corpus, orient=tk.VERTICAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        tree_frame = ttk.Frame(paned)
        paned.add(tree_frame, weight=3)

        columns = ("target", "orig", "sz_7z", "sz_v1", "sz_latest", "delta_7z", "delta_v1", "enc_7z", "enc_v1", "enc_latest", "dec_7z", "dec_v1", "dec_latest", "verify")
        self.bench_tree = ttk.Treeview(tree_frame, columns=columns, show="headings", selectmode="browse")

        headers = {
            "target": "Target",
            "orig": "Orig",
            "sz_7z": "7z Size",
            "sz_v1": "v1 Size",
            "sz_latest": "Latest Size",
            "delta_7z": "vs 7z Δ%",
            "delta_v1": "vs v1 Δ%",
            "enc_7z": "7z Enc",
            "enc_v1": "v1 Enc",
            "enc_latest": "Latest Enc",
            "dec_7z": "7z Dec",
            "dec_v1": "v1 Dec",
            "dec_latest": "Latest Dec",
            "verify": "Verify"
        }
        for col, txt in headers.items():
            self.bench_tree.heading(col, text=txt, command=lambda c=col: self.bench_sort_column(c, False))
            self.bench_tree.column(col, width=85, anchor=tk.CENTER)
        self.bench_tree.column("target", width=110, anchor=tk.W)

        b_scrollbar_y = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.bench_tree.yview)
        b_scrollbar_x = ttk.Scrollbar(tree_frame, orient=tk.HORIZONTAL, command=self.bench_tree.xview)
        self.bench_tree.configure(yscrollcommand=b_scrollbar_y.set, xscrollcommand=b_scrollbar_x.set)

        self.bench_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        b_scrollbar_y.pack(side=tk.RIGHT, fill=tk.Y)
        b_scrollbar_x.pack(side=tk.BOTTOM, fill=tk.X)

        self.bench_tree.tag_configure("win_all", background=self.win_bg, foreground=self.win_fg)

        bottom_frame = ttk.Frame(paned)
        paned.add(bottom_frame, weight=2)

        self.bench_summary_label = ttk.Label(bottom_frame, text="Load benchmark JSON to view comparative results.", font=("Segoe UI", 10, "bold"))
        self.bench_summary_label.pack(fill=tk.X, padx=5, pady=2)

        self.chart_canvas = tk.Canvas(bottom_frame, bg=self.chart_bg, highlightthickness=0, height=180)
        self.chart_canvas.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        self.chart_canvas.bind("<Configure>", lambda e: self.draw_benchmark_chart())

        self.benchmark_raw_data = []
        self.load_benchmark_data()

        # Sub-tab 1: Custom Benchmarks & Archive Compare
        ctb1 = ttk.Frame(self.tab_bench_custom)
        ctb1.pack(fill=tk.X, padx=5, pady=(5, 2))

        ttk.Button(ctb1, text="⚡ Quick Test File...", style="Accent.TButton", command=lambda: self.run_quick_test(target_type="file")).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(ctb1, text="📁 Quick Test Folder...", command=lambda: self.run_quick_test(target_type="folder")).pack(side=tk.LEFT, padx=4)

        ttk.Label(ctb1, text="Preset:").pack(side=tk.LEFT, padx=(10, 4))
        self.qt_preset_var = tk.StringVar(value="Normal - Default")
        ttk.Combobox(
            ctb1,
            textvariable=self.qt_preset_var,
            values=["Normal - Default", "Ultra (Max)", "Fast", "Compare Both (Normal vs Fast)"],
            state="readonly",
            width=24,
        ).pack(side=tk.LEFT, padx=(0, 8))

        ttk.Button(ctb1, text="📥 Import .prism Archive(s)...", command=self.import_archives_dialog).pack(side=tk.LEFT, padx=4)

        self.qt_disk_extract_var = tk.BooleanVar(value=False)
        chk_disk = ttk.Checkbutton(ctb1, text="Include Disk Extract", variable=self.qt_disk_extract_var)
        chk_disk.pack(side=tk.LEFT, padx=(10, 4))
        ToolTip(
            chk_disk,
            "Unchecked = fast in-memory decode + SHA-256 (avoids Windows Defender %TEMP% scan throttling); "
            "checked = full disk extraction to temp folder.",
        )

        ctb2 = ttk.Frame(self.tab_bench_custom)
        ctb2.pack(fill=tk.X, padx=5, pady=(2, 5))

        ttk.Button(ctb2, text="⏱ Measure Decode", command=self.measure_selected_custom_decode).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(ctb2, text="🎯 Set as Baseline", command=self.set_selected_custom_baseline).pack(side=tk.LEFT, padx=4)
        ttk.Button(ctb2, text="🗑 Remove Selected", command=self.remove_selected_custom_benchmarks).pack(side=tk.LEFT, padx=4)
        ttk.Button(ctb2, text="🧹 Clear All", command=self.clear_custom_benchmarks).pack(side=tk.LEFT, padx=4)

        ttk.Label(ctb2, text="Metric:").pack(side=tk.LEFT, padx=(16, 4))
        self.custom_metric_var = tk.StringVar(value="Compressed Size")
        custom_metric_combo = ttk.Combobox(
            ctb2,
            textvariable=self.custom_metric_var,
            values=["Compressed Size", "Encode Speed", "Decode Speed", "Size Savings %"],
            state="readonly",
            width=18,
        )
        custom_metric_combo.pack(side=tk.LEFT, padx=2)
        custom_metric_combo.bind("<<ComboboxSelected>>", lambda e: self.draw_custom_benchmark_chart())

        c_paned = ttk.PanedWindow(self.tab_bench_custom, orient=tk.VERTICAL)
        c_paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=4)

        c_tree_frame = ttk.Frame(c_paned)
        c_paned.add(c_tree_frame, weight=3)

        c_cols = (
            "label", "source_type", "preset", "algorithm", "orig", "comp", "ratio",
            "savings", "vs_base_sz", "enc_time", "enc_speed", "dec_time", "dec_speed", "verified", "created"
        )
        self.custom_bench_tree = ttk.Treeview(c_tree_frame, columns=c_cols, show="headings", selectmode="extended")
        c_headers = {
            "label": "Label / Target",
            "source_type": "Source",
            "preset": "Preset",
            "algorithm": "Algorithm",
            "orig": "Original",
            "comp": "Compressed",
            "ratio": "Ratio",
            "savings": "Savings",
            "vs_base_sz": "vs Base Size",
            "enc_time": "Enc Time",
            "enc_speed": "Enc Speed",
            "dec_time": "Dec Time",
            "dec_speed": "Dec Speed",
            "verified": "Verified",
            "created": "Created (UTC)",
        }
        c_widths = {
            "label": 160, "source_type": 80, "preset": 95, "algorithm": 150,
            "orig": 88, "comp": 92, "ratio": 68, "savings": 72, "vs_base_sz": 92,
            "enc_time": 74, "enc_speed": 96, "dec_time": 74, "dec_speed": 96,
            "verified": 62, "created": 130,
        }
        for col, txt in c_headers.items():
            self.custom_bench_tree.heading(col, text=txt, command=lambda c=col: self.custom_bench_sort_column(c, False))
            anchor = tk.W if col in ("label", "algorithm", "preset") else tk.CENTER
            self.custom_bench_tree.column(col, width=c_widths.get(col, 85), anchor=anchor)

        c_scroll_y = ttk.Scrollbar(c_tree_frame, orient=tk.VERTICAL, command=self.custom_bench_tree.yview)
        c_scroll_x = ttk.Scrollbar(c_tree_frame, orient=tk.HORIZONTAL, command=self.custom_bench_tree.xview)
        self.custom_bench_tree.configure(yscrollcommand=c_scroll_y.set, xscrollcommand=c_scroll_x.set)

        self.custom_bench_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        c_scroll_y.pack(side=tk.RIGHT, fill=tk.Y)
        c_scroll_x.pack(side=tk.BOTTOM, fill=tk.X)

        self.custom_bench_tree.tag_configure("baseline", background=self.win_bg, foreground=self.win_fg)
        self.custom_bench_tree.bind("<<TreeviewSelect>>", lambda e: self._on_custom_bench_select())
        self.custom_bench_tree.bind("<Double-1>", lambda e: self.set_selected_custom_baseline())

        c_bottom = ttk.Frame(c_paned)
        c_paned.add(c_bottom, weight=2)

        self.custom_bench_summary_label = ttk.Label(
            c_bottom,
            text="No custom benchmarks yet. Run Quick Test or Import .prism Archive(s) to compare.",
            font=("Segoe UI", 10, "bold"),
        )
        self.custom_bench_summary_label.pack(fill=tk.X, padx=5, pady=2)

        self.custom_chart_canvas = tk.Canvas(c_bottom, bg=self.chart_bg, highlightthickness=0, height=180)
        self.custom_chart_canvas.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        self.custom_chart_canvas.bind("<Configure>", lambda e: self.draw_custom_benchmark_chart())

        self.load_custom_benchmarks()
        self.refresh_custom_benchmarks_ui()

    def bench_sort_column(self, col, reverse):
        l = [(self.bench_tree.set(k, col), k) for k in self.bench_tree.get_children('')]
        l.sort(reverse=reverse)
        for index, (val, k) in enumerate(l):
            self.bench_tree.move(k, '', index)
        self.bench_tree.heading(col, command=lambda: self.bench_sort_column(col, not reverse))

    def load_benchmark_data(self):
        for item in self.bench_tree.get_children():
            self.bench_tree.delete(item)
            
        json_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prism_benchmark_results.json")
        if not os.path.exists(json_path):
            self.bench_summary_label.config(text="Benchmark results JSON not found.")
            return
            
        try:
            with open(json_path, "r", encoding="utf-8") as f:
                self.benchmark_raw_data = json.load(f)
                
            m_div, _, m_lbl, _, speed_lbl = self._unit_labels()
            speed_scale = 1048576.0 / m_div

            total_sz_7z = 0
            total_sz_v1 = 0
            total_sz_latest = 0
            enc_speeds_latest = []
            dec_speeds_latest = []
            wins = 0
            verified_count = 0
            
            for r in self.benchmark_raw_data:
                t_name = r["target"]
                orig_bytes = r.get("orig_bytes", r.get("orig_mib", 0) * 1048576.0)
                orig = f"{orig_bytes / m_div:.1f} {m_lbl}"
                
                sz_7z_val = r["7z"]["size"]
                sz_7z = f"{sz_7z_val / m_div:.2f} {m_lbl}"
                total_sz_7z += sz_7z_val
                
                v1_dict = r.get("prism_v1", r["prism"])
                sz_v1_val = v1_dict["size"]
                sz_v1 = f"{sz_v1_val / m_div:.2f} {m_lbl}"
                total_sz_v1 += sz_v1_val
                
                latest_dict = r["prism"]
                sz_latest_val = latest_dict["size"]
                sz_latest = f"{sz_latest_val / m_div:.2f} {m_lbl}"
                total_sz_latest += sz_latest_val
                
                delta_7z = ((sz_latest_val - sz_7z_val) / sz_7z_val) * 100 if sz_7z_val > 0 else 0
                delta_v1 = ((sz_latest_val - sz_v1_val) / sz_v1_val) * 100 if sz_v1_val > 0 else 0
                
                delta_7z_str = f"{delta_7z:+.1f}%"
                delta_v1_str = f"{delta_v1:+.1f}%"
                
                e_7z = f"{r['7z']['enc_mib_s'] * speed_scale:.1f}"
                e_v1 = f"{v1_dict['enc_mib_s'] * speed_scale:.1f}"
                e_latest = f"{latest_dict['enc_mib_s'] * speed_scale:.1f}"
                enc_speeds_latest.append(latest_dict['enc_mib_s'] * speed_scale)
                
                d_7z = f"{r['7z']['dec_mib_s'] * speed_scale:.1f}"
                d_v1 = f"{v1_dict['dec_mib_s'] * speed_scale:.1f}"
                d_latest = f"{latest_dict['dec_mib_s'] * speed_scale:.1f}"
                dec_speeds_latest.append(latest_dict['dec_mib_s'] * speed_scale)
                
                v = "OK" if latest_dict["verified"] else "FAIL"
                if latest_dict["verified"]:
                    verified_count += 1
                    
                is_win_all = latest_dict["size"] < sz_7z_val and latest_dict["enc_seconds"] < r["7z"]["enc_seconds"] and latest_dict["dec_seconds"] < r["7z"]["dec_seconds"]
                if is_win_all:
                    wins += 1
                    tag = "win_all"
                else:
                    tag = ""
                    
                row_vals = (t_name, orig, sz_7z, sz_v1, sz_latest, delta_7z_str, delta_v1_str, e_7z, e_v1, e_latest, d_7z, d_v1, d_latest, v)
                self.bench_tree.insert("", tk.END, values=row_vals, tags=(tag,))
                
            avg_enc = sum(enc_speeds_latest) / len(enc_speeds_latest) if enc_speeds_latest else 0
            avg_dec = sum(dec_speeds_latest) / len(dec_speeds_latest) if dec_speeds_latest else 0
            
            summary_txt = (
                f"Total Size: 7z={total_sz_7z / m_div:.1f} {m_lbl} | v1={total_sz_v1 / m_div:.1f} {m_lbl} | Latest={total_sz_latest / m_div:.1f} {m_lbl}  |  "
                f"Avg Speed: Enc {avg_enc:.1f} {speed_lbl}, Dec {avg_dec:.1f} {speed_lbl}  |  "
                f"{wins}/{len(self.benchmark_raw_data)} All-Metric Wins | {verified_count}/{len(self.benchmark_raw_data)} Verified"
            )
            self.bench_summary_label.config(text=summary_txt)
            self.draw_benchmark_chart()
        except Exception as e:
            self.bench_summary_label.config(text=f"Error loading benchmark JSON: {e}")

    def draw_benchmark_chart(self):
        self.chart_canvas.delete("all")
        if not self.benchmark_raw_data:
            return
            
        width = self.chart_canvas.winfo_width()
        height = self.chart_canvas.winfo_height()
        if width < 100:
            width = 900
        if height < 50:
            height = 180
            
        metric = self.bench_metric_var.get()
        m_div, _, m_lbl, _, speed_lbl = self._unit_labels()
        speed_scale = 1048576.0 / m_div

        n = len(self.benchmark_raw_data)
        margin_left = 60
        margin_right = 30
        margin_top = 28
        margin_bottom = 40
        
        plot_w = width - margin_left - margin_right
        plot_h = height - margin_top - margin_bottom
        
        vals_7z, vals_v1, vals_latest, labels = [], [], [], []
        
        for r in self.benchmark_raw_data:
            labels.append(r["target"])
            v1_dict = r.get("prism_v1", r["prism"])
            latest_dict = r["prism"]
            
            if "Compressed Size" in metric:
                vals_7z.append(r["7z"]["size"] / m_div)
                vals_v1.append(v1_dict["size"] / m_div)
                vals_latest.append(latest_dict["size"] / m_div)
            elif "Encode Speed" in metric:
                vals_7z.append(r["7z"]["enc_mib_s"] * speed_scale)
                vals_v1.append(v1_dict["enc_mib_s"] * speed_scale)
                vals_latest.append(latest_dict["enc_mib_s"] * speed_scale)
            else:
                vals_7z.append(r["7z"]["dec_mib_s"] * speed_scale)
                vals_v1.append(v1_dict["dec_mib_s"] * speed_scale)
                vals_latest.append(latest_dict["dec_mib_s"] * speed_scale)
                
        all_vals = vals_7z + vals_v1 + vals_latest
        max_val = max(all_vals) if all_vals else 1.0
        if max_val == 0:
            max_val = 1.0

        unit_txt = m_lbl if "Compressed Size" in metric else speed_lbl
        self.chart_canvas.create_text(margin_left - 8, 6, text=unit_txt, fill=self.fg_muted, anchor=tk.NE, font=("Segoe UI", 8, "bold"))
            
        self.chart_canvas.create_line(margin_left, height - margin_bottom, width - margin_right, height - margin_bottom, fill=self.chart_axis, width=2)
        self.chart_canvas.create_line(margin_left, margin_top, margin_left, height - margin_bottom, fill=self.chart_axis, width=2)
        
        for i in range(5):
            y_val = max_val * (i / 4.0)
            y_pos = height - margin_bottom - (plot_h * (i / 4.0))
            self.chart_canvas.create_line(margin_left, y_pos, width - margin_right, y_pos, fill=self.chart_grid, dash=(2, 2))
            self.chart_canvas.create_text(margin_left - 8, y_pos, text=f"{y_val:.1f}", fill=self.chart_text, anchor=tk.E, font=("Segoe UI", 8))
            
        group_w = plot_w / n if n > 0 else plot_w
        bar_w = group_w / 3.6
        colors = {"7z": "#777777", "v1": "#d97706", "latest": self.accent}
        
        for idx, lbl in enumerate(labels):
            gx = margin_left + (idx * group_w) + (group_w / 2.0)
            
            h_7z = (vals_7z[idx] / max_val) * plot_h
            self.chart_canvas.create_rectangle(gx - bar_w * 1.5, height - margin_bottom - h_7z, gx - bar_w * 0.5, height - margin_bottom, fill=colors["7z"], outline="")
            
            h_v1 = (vals_v1[idx] / max_val) * plot_h
            self.chart_canvas.create_rectangle(gx - bar_w * 0.5, height - margin_bottom - h_v1, gx + bar_w * 0.5, height - margin_bottom, fill=colors["v1"], outline="")
            
            h_lat = (vals_latest[idx] / max_val) * plot_h
            self.chart_canvas.create_rectangle(gx + bar_w * 0.5, height - margin_bottom - h_lat, gx + bar_w * 1.5, height - margin_bottom, fill=colors["latest"], outline="")
            
            self.chart_canvas.create_text(gx, height - margin_bottom + 12, text=lbl, fill=self.chart_text, anchor=tk.N, font=("Segoe UI", 8), angle=15)
            
        lx = max(margin_left, width - margin_right - 270)
        ly = margin_top + 10
        self.chart_canvas.create_rectangle(lx, ly, lx + 12, ly + 12, fill=colors["7z"], outline="")
        self.chart_canvas.create_text(lx + 18, ly + 6, text="7z -mx=9", fill=self.chart_text, anchor=tk.W, font=("Segoe UI", 9))
        
        self.chart_canvas.create_rectangle(lx + 85, ly, lx + 97, ly + 12, fill=colors["v1"], outline="")
        self.chart_canvas.create_text(lx + 103, ly + 6, text="PRISM v1", fill=self.chart_text, anchor=tk.W, font=("Segoe UI", 9))
        
        self.chart_canvas.create_rectangle(lx + 170, ly, lx + 182, ly + 12, fill=colors["latest"], outline="")
        self.chart_canvas.create_text(lx + 188, ly + 6, text="PRISM Latest", fill=self.chart_text, anchor=tk.W, font=("Segoe UI", 9))

    # -------------------- Custom Benchmarks & Archive Compare --------------------
    def load_custom_benchmarks(self):
        self.custom_benchmarks = []
        self.custom_bench_baseline_id = None
        if not self.custom_bench_file.exists():
            return
        try:
            with open(self.custom_bench_file, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict):
                self.custom_bench_baseline_id = data.get("baseline_id")
                items = data.get("items", [])
                if isinstance(items, list):
                    self.custom_benchmarks = [it for it in items if isinstance(it, dict)]
            elif isinstance(data, list):
                self.custom_benchmarks = [it for it in data if isinstance(it, dict)]
        except Exception:
            self.custom_benchmarks = []
            self.custom_bench_baseline_id = None

    def save_custom_benchmarks(self):
        try:
            ids = {r.get("id") for r in self.custom_benchmarks}
            if self.custom_bench_baseline_id not in ids:
                self.custom_bench_baseline_id = None
            payload = {
                "baseline_id": self.custom_bench_baseline_id,
                "items": self.custom_benchmarks,
            }
            with open(self.custom_bench_file, "w", encoding="utf-8") as f:
                json.dump(payload, f, indent=2)
        except Exception:
            pass

    @staticmethod
    def _make_custom_bench_record(
        rec_id=None,
        label="",
        source_type="QuickTest",
        preset="Ultra",
        algorithm="Standard",
        orig_bytes=0,
        comp_bytes=0,
        enc_ms=None,
        dec_ms=None,
        verified=None,
        archive_path="",
        created_utc="",
        has_pmet=False,
        is_dir=False,
    ):
        return {
            "id": rec_id or uuid.uuid4().hex[:12],
            "label": str(label),
            "source_type": str(source_type),
            "preset": str(preset),
            "algorithm": str(algorithm),
            "orig_bytes": int(orig_bytes or 0),
            "comp_bytes": int(comp_bytes or 0),
            "enc_ms": float(enc_ms) if enc_ms is not None else None,
            "dec_ms": float(dec_ms) if dec_ms is not None else None,
            "verified": verified if verified in (True, False, None) else None,
            "archive_path": str(archive_path or ""),
            "created_utc": str(created_utc or ""),
            "has_pmet": bool(has_pmet),
            "is_dir": bool(is_dir),
        }

    def import_archives_to_benchmark(self, paths):
        imported_ids = []
        errors = []
        existing_by_path = {}
        for idx, rec in enumerate(self.custom_benchmarks):
            ap = rec.get("archive_path", "")
            if ap and rec.get("source_type") == "Imported":
                existing_by_path[os.path.normcase(os.path.abspath(ap))] = idx

        for raw_p in paths:
            if not raw_p or not os.path.exists(raw_p):
                continue
            abs_p = os.path.abspath(raw_p)
            norm_p = os.path.normcase(abs_p)
            try:
                info = prism.read_metadata(abs_p)
                has_pmet = bool(info.get("has_pmet") and isinstance(info.get("pmet"), dict))
                pm = info.get("pmet") if has_pmet else {}
                orig_bytes = int((pm.get("orig_bytes") if has_pmet else None) or info.get("orig_bytes") or 0)
                comp_bytes = int((pm.get("archive_bytes") if has_pmet else None) or info.get("archive_bytes") or os.path.getsize(abs_p))
                algorithm = (pm.get("algorithm") if has_pmet else None) or info.get("algorithm") or "Standard"
                is_dir = bool((pm.get("is_dir") if has_pmet else None) or info.get("is_dir"))
                if has_pmet:
                    preset = pm.get("preset") or "Normal"
                    enc_ms = float(pm["enc_ms"]) if pm.get("enc_ms") is not None else None
                    created_utc = pm.get("created_utc") or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                else:
                    preset = "Unknown (Legacy)"
                    enc_ms = None
                    created_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(os.path.getmtime(abs_p)))

                prev_idx = existing_by_path.get(norm_p)
                prev_rec = self.custom_benchmarks[prev_idx] if prev_idx is not None else None
                rec = self._make_custom_bench_record(
                    rec_id=prev_rec.get("id") if prev_rec else None,
                    label=os.path.basename(abs_p),
                    source_type="Imported",
                    preset=preset,
                    algorithm=algorithm,
                    orig_bytes=orig_bytes,
                    comp_bytes=comp_bytes,
                    enc_ms=enc_ms,
                    dec_ms=prev_rec.get("dec_ms") if (prev_rec and prev_rec.get("comp_bytes") == comp_bytes) else None,
                    verified=prev_rec.get("verified") if (prev_rec and prev_rec.get("comp_bytes") == comp_bytes) else None,
                    archive_path=abs_p,
                    created_utc=created_utc,
                    has_pmet=has_pmet,
                    is_dir=is_dir,
                )
                if prev_idx is not None:
                    self.custom_benchmarks[prev_idx] = rec
                else:
                    self.custom_benchmarks.append(rec)
                    existing_by_path[norm_p] = len(self.custom_benchmarks) - 1
                imported_ids.append(rec["id"])
            except Exception as e:
                errors.append(f"{os.path.basename(abs_p)}: {e}")

        if imported_ids:
            self.save_custom_benchmarks()
            self.refresh_custom_benchmarks_ui()
            existing_ids = [rid for rid in imported_ids if self.custom_bench_tree.exists(rid)]
            if existing_ids:
                self.custom_bench_tree.selection_set(existing_ids)
                self.update_custom_benchmark_summary()
                self.draw_custom_benchmark_chart()
            self.log_activity(f"Imported {len(imported_ids)} archive(s) to Custom Benchmarks.")
        if errors:
            messagebox.showwarning("Import Warnings", "Some files could not be imported:\n" + "\n".join(errors[:5]))
        return imported_ids

    def import_archives_dialog(self):
        files = filedialog.askopenfilenames(
            title="Import .prism Archive(s)",
            filetypes=[("PRISM Archives", "*.prism"), ("All Files", "*.*")],
        )
        if not files:
            return
        imported_ids = self.import_archives_to_benchmark(files)
        if imported_ids:
            self.bench_sub_notebook.select(self.tab_bench_custom)

    def _on_custom_bench_select(self):
        self.update_custom_benchmark_summary()
        self.draw_custom_benchmark_chart()

    def refresh_custom_benchmarks_ui(self):
        if not hasattr(self, "custom_bench_tree"):
            return
        prev_sel = list(self.custom_bench_tree.selection())
        for item in self.custom_bench_tree.get_children():
            self.custom_bench_tree.delete(item)

        valid_ids = {r.get("id") for r in self.custom_benchmarks}
        if self.custom_bench_baseline_id not in valid_ids:
            self.custom_bench_baseline_id = None

        base_rec = next((r for r in self.custom_benchmarks if r.get("id") == self.custom_bench_baseline_id), None)
        base_comp = int(base_rec.get("comp_bytes", 0)) if base_rec else 0

        for rec in self.custom_benchmarks:
            rid = rec.get("id") or uuid.uuid4().hex[:12]
            rec["id"] = rid
            is_base = (rid == self.custom_bench_baseline_id)
            orig_b = int(rec.get("orig_bytes", 0))
            comp_b = int(rec.get("comp_bytes", 0))
            ratio_pct = (comp_b / orig_b * 100.0) if orig_b > 0 else None
            savings_pct = (100.0 - ratio_pct) if ratio_pct is not None else None

            if is_base:
                vs_base_str = "Baseline"
            elif base_rec is not None and base_comp > 0:
                delta_pct = ((comp_b - base_comp) / base_comp) * 100.0
                vs_base_str = f"{delta_pct:+.2f}%"
            else:
                vs_base_str = "—"

            enc_ms = rec.get("enc_ms")
            dec_ms = rec.get("dec_ms")
            enc_t_str = f"{enc_ms / 1000.0:.2f}s" if enc_ms is not None else "—"
            enc_s_str = self.format_speed_from_bytes(orig_b, enc_ms / 1000.0) if (enc_ms is not None and enc_ms > 0 and orig_b > 0) else "—"
            dec_t_str = f"{dec_ms / 1000.0:.2f}s" if dec_ms is not None else "—"
            dec_s_str = self.format_speed_from_bytes(orig_b, dec_ms / 1000.0) if (dec_ms is not None and dec_ms > 0 and orig_b > 0) else "—"

            v_val = rec.get("verified")
            v_str = "OK" if v_val is True else ("FAIL" if v_val is False else "—")
            created_raw = rec.get("created_utc", "")
            created_str = created_raw[:19].replace("T", " ") if created_raw else "—"
            label_str = f"★ {rec.get('label', '')}" if is_base else rec.get("label", "")

            vals = (
                label_str,
                rec.get("source_type", "QuickTest"),
                rec.get("preset", "—"),
                rec.get("algorithm", "Standard"),
                self.format_size(orig_b),
                self.format_size(comp_b),
                f"{ratio_pct:.2f}%" if ratio_pct is not None else "—",
                f"{savings_pct:.2f}%" if savings_pct is not None else "—",
                vs_base_str,
                enc_t_str,
                enc_s_str,
                dec_t_str,
                dec_s_str,
                v_str,
                created_str,
            )
            tags = ("baseline",) if is_base else ()
            self.custom_bench_tree.insert("", tk.END, iid=rid, values=vals, tags=tags)

        keep_sel = [rid for rid in prev_sel if self.custom_bench_tree.exists(rid)]
        if keep_sel:
            self.custom_bench_tree.selection_set(keep_sel)

        self.update_custom_benchmark_summary()
        self.draw_custom_benchmark_chart()

    def update_custom_benchmark_summary(self):
        if not hasattr(self, "custom_bench_summary_label"):
            return
        n = len(self.custom_benchmarks)
        if n == 0:
            self.custom_bench_summary_label.config(
                text="No custom benchmarks yet. Run Quick Test or Import .prism Archive(s) to compare."
            )
            return
        if n == 1:
            r = self.custom_benchmarks[0]
            orig_b = int(r.get("orig_bytes", 0))
            comp_b = int(r.get("comp_bytes", 0))
            ratio_pct = (comp_b / orig_b * 100.0) if orig_b > 0 else 0.0
            enc_ms = r.get("enc_ms")
            enc_part = f" | Enc: {self.format_speed_from_bytes(orig_b, enc_ms / 1000.0)}" if (enc_ms and enc_ms > 0 and orig_b > 0) else ""
            self.custom_bench_summary_label.config(
                text=f"1 benchmark ({r.get('label', '')} [{r.get('preset', '')}]): {self.format_size(comp_b)} / {self.format_size(orig_b)} ({ratio_pct:.2f}%){enc_part} — add/import another to compare."
            )
            return

        m_div, _, m_lbl, _, speed_lbl = self._unit_labels()
        rec_by_id = {r["id"]: r for r in self.custom_benchmarks if r.get("id")}
        sel_ids = [i for i in self.custom_bench_tree.selection() if i in rec_by_id]

        if len(sel_ids) >= 2:
            base_rec = next((rec_by_id[i] for i in sel_ids if i == self.custom_bench_baseline_id), rec_by_id[sel_ids[0]])
            cmp_rec = next((rec_by_id[i] for i in sel_ids if rec_by_id[i]["id"] != base_rec["id"]), rec_by_id[sel_ids[1]])
        else:
            base_rec = rec_by_id.get(self.custom_bench_baseline_id) or self.custom_benchmarks[0]
            if len(sel_ids) == 1 and sel_ids[0] != base_rec["id"]:
                cmp_rec = rec_by_id[sel_ids[0]]
            else:
                cmp_rec = next((r for r in reversed(self.custom_benchmarks) if r["id"] != base_rec["id"]), self.custom_benchmarks[-1])

        b_comp = int(base_rec.get("comp_bytes", 0))
        c_comp = int(cmp_rec.get("comp_bytes", 0))
        diff_b = c_comp - b_comp
        diff_sign = "-" if diff_b < 0 else "+"
        diff_pct = (diff_b / b_comp * 100.0) if b_comp > 0 else 0.0

        parts = [
            f"Compare ({cmp_rec.get('label', '')} [{cmp_rec.get('preset', '')}] vs {base_rec.get('label', '')} [{base_rec.get('preset', '')}]): "
            f"Size {diff_sign}{self.format_size(abs(diff_b))} ({diff_pct:+.2f}%)"
        ]
        b_orig = int(base_rec.get("orig_bytes", 0))
        c_orig = int(cmp_rec.get("orig_bytes", 0))
        b_enc, c_enc = base_rec.get("enc_ms"), cmp_rec.get("enc_ms")
        if b_enc and c_enc and b_enc > 0 and c_enc > 0 and b_orig > 0 and c_orig > 0:
            spd_b = (b_orig / m_div) / (b_enc / 1000.0)
            spd_c = (c_orig / m_div) / (c_enc / 1000.0)
            enc_ratio = spd_c / spd_b if spd_b > 0 else 0.0
            parts.append(f"Enc: {spd_c:.2f} vs {spd_b:.2f} {speed_lbl} ({enc_ratio:.2f}x)")

        b_dec, c_dec = base_rec.get("dec_ms"), cmp_rec.get("dec_ms")
        if b_dec and c_dec and b_dec > 0 and c_dec > 0 and b_orig > 0 and c_orig > 0:
            dspd_b = (b_orig / m_div) / (b_dec / 1000.0)
            dspd_c = (c_orig / m_div) / (c_dec / 1000.0)
            dec_ratio = dspd_c / dspd_b if dspd_b > 0 else 0.0
            parts.append(f"Dec: {dspd_c:.2f} vs {dspd_b:.2f} {speed_lbl} ({dec_ratio:.2f}x)")

        self.custom_bench_summary_label.config(text="  |  ".join(parts))

    def draw_custom_benchmark_chart(self):
        if not hasattr(self, "custom_chart_canvas"):
            return
        self.custom_chart_canvas.delete("all")
        width = self.custom_chart_canvas.winfo_width()
        height = self.custom_chart_canvas.winfo_height()
        if width < 100:
            width = 900
        if height < 50:
            height = 180

        if not self.custom_benchmarks:
            self.custom_chart_canvas.create_text(
                width / 2, height / 2,
                text="No custom benchmarks to plot yet.",
                fill=self.fg_muted, font=("Segoe UI", 10),
            )
            return

        rec_by_id = {r["id"]: r for r in self.custom_benchmarks if r.get("id")}
        sel_ids = [i for i in self.custom_bench_tree.selection() if i in rec_by_id]
        if len(sel_ids) >= 2:
            records = [rec_by_id[i] for i in sel_ids]
        else:
            records = self.custom_benchmarks[-12:]

        metric = self.custom_metric_var.get()
        k_div, m_div, _, k_lbl, m_lbl, _, speed_lbl = self._get_unit_spec()
        use_kb_size = (metric == "Compressed Size" and max((int(r.get("comp_bytes", 0)) for r in records), default=0) < m_div)
        size_div = k_div if use_kb_size else m_div
        size_lbl = k_lbl if use_kb_size else m_lbl

        vals = []
        labels = []
        bar_colors = []
        for r in records:
            orig_b = int(r.get("orig_bytes", 0))
            comp_b = int(r.get("comp_bytes", 0))
            if metric == "Compressed Size":
                vals.append(comp_b / size_div)
            elif metric == "Encode Speed":
                enc_ms = r.get("enc_ms")
                vals.append((orig_b / m_div) / (enc_ms / 1000.0) if (enc_ms and enc_ms > 0 and orig_b > 0) else None)
            elif metric == "Decode Speed":
                dec_ms = r.get("dec_ms")
                vals.append((orig_b / m_div) / (dec_ms / 1000.0) if (dec_ms and dec_ms > 0 and orig_b > 0) else None)
            else:
                vals.append((100.0 - (comp_b / orig_b * 100.0)) if orig_b > 0 else 0.0)

            short_name = r.get("label", "")
            if len(short_name) > 16:
                short_name = short_name[:13] + "..."
            preset_tag = r.get("preset", "")
            if "Unknown" in preset_tag:
                preset_tag = "Legacy"
            prefix = "★ " if r.get("id") == self.custom_bench_baseline_id else ""
            labels.append(f"{prefix}{short_name}\n[{preset_tag}]")

            if r.get("id") == self.custom_bench_baseline_id:
                bar_colors.append("#10b981")
            elif "Fast" in preset_tag:
                bar_colors.append("#f59e0b")
            elif "Legacy" in preset_tag:
                bar_colors.append("#8b5cf6")
            else:
                bar_colors.append(self.accent)

        numeric_vals = [max(0.0, v) for v in vals if v is not None]
        if not numeric_vals:
            self.custom_chart_canvas.create_text(
                width / 2, height / 2,
                text=f"No {metric} measurements available for selected items (run Measure Decode first).",
                fill=self.fg_muted, font=("Segoe UI", 10),
            )
            return

        max_val = max(numeric_vals)
        if max_val <= 0:
            max_val = 1.0

        unit_lbl = size_lbl if metric == "Compressed Size" else ("%" if metric == "Size Savings %" else speed_lbl)
        margin_left = 65
        margin_right = 30
        margin_top = 34
        margin_bottom = 48
        plot_w = max(50, width - margin_left - margin_right)
        plot_h = max(30, height - margin_top - margin_bottom)

        self.custom_chart_canvas.create_text(margin_left - 8, 6, text=unit_lbl, fill=self.fg_muted, anchor=tk.NE, font=("Segoe UI", 8, "bold"))
        self.custom_chart_canvas.create_line(margin_left, height - margin_bottom, width - margin_right, height - margin_bottom, fill=self.chart_axis, width=2)
        self.custom_chart_canvas.create_line(margin_left, margin_top, margin_left, height - margin_bottom, fill=self.chart_axis, width=2)

        for i in range(5):
            y_val = max_val * (i / 4.0)
            y_pos = height - margin_bottom - (plot_h * (i / 4.0))
            self.custom_chart_canvas.create_line(margin_left, y_pos, width - margin_right, y_pos, fill=self.chart_grid, dash=(2, 2))
            self.custom_chart_canvas.create_text(margin_left - 8, y_pos, text=f"{y_val:.1f}", fill=self.chart_text, anchor=tk.E, font=("Segoe UI", 8))

        n = len(records)
        slot_w = plot_w / n
        bar_w = min(68.0, max(16.0, slot_w * 0.56))

        for idx, val in enumerate(vals):
            cx = margin_left + (idx * slot_w) + (slot_w / 2.0)
            if val is None:
                self.custom_chart_canvas.create_text(cx, height - margin_bottom - 10, text="N/A", fill=self.fg_muted, anchor=tk.S, font=("Segoe UI", 8))
            else:
                bar_h = (max(0.0, val) / max_val) * plot_h
                y_top = height - margin_bottom - bar_h
                self.custom_chart_canvas.create_rectangle(
                    cx - bar_w / 2.0, y_top,
                    cx + bar_w / 2.0, height - margin_bottom,
                    fill=bar_colors[idx], outline="",
                )
                ann = f"{val:.1f}%" if metric == "Size Savings %" else f"{val:.2f}"
                self.custom_chart_canvas.create_text(
                    cx, max(16, y_top - 4),
                    text=ann, fill=self.chart_text, anchor=tk.S, font=("Segoe UI", 8, "bold"),
                )
            self.custom_chart_canvas.create_text(
                cx, height - margin_bottom + 8,
                text=labels[idx], fill=self.chart_text, anchor=tk.N, font=("Segoe UI", 8), justify=tk.CENTER,
            )

    def set_selected_custom_baseline(self):
        sel = self.custom_bench_tree.selection()
        if not sel:
            messagebox.showinfo("Baseline", "Select a benchmark row to set as the comparison baseline.")
            return
        self.custom_bench_baseline_id = sel[0]
        self.save_custom_benchmarks()
        self.refresh_custom_benchmarks_ui()

    def remove_selected_custom_benchmarks(self):
        sel = set(self.custom_bench_tree.selection())
        if not sel:
            return
        self.custom_benchmarks = [r for r in self.custom_benchmarks if r.get("id") not in sel]
        if self.custom_bench_baseline_id in sel:
            self.custom_bench_baseline_id = None
        self.save_custom_benchmarks()
        self.refresh_custom_benchmarks_ui()

    def clear_custom_benchmarks(self):
        if not self.custom_benchmarks:
            return
        self.custom_benchmarks = []
        self.custom_bench_baseline_id = None
        self.save_custom_benchmarks()
        self.refresh_custom_benchmarks_ui()
        self.log_activity("Cleared custom benchmarks.")

    def custom_bench_sort_column(self, col, reverse):
        num_cols = {"orig", "comp", "ratio", "savings", "vs_base_sz", "enc_time", "enc_speed", "dec_time", "dec_speed"}
        rec_by_id = {r["id"]: r for r in self.custom_benchmarks if r.get("id")}
        base_rec = rec_by_id.get(self.custom_bench_baseline_id)
        base_comp = int(base_rec.get("comp_bytes", 0)) if base_rec else 0

        def _sort_key(iid):
            r = rec_by_id.get(iid)
            if not r or col not in num_cols:
                return (0, str(self.custom_bench_tree.set(iid, col)).lower())
            orig_b = int(r.get("orig_bytes", 0))
            comp_b = int(r.get("comp_bytes", 0))
            val = None
            if col == "orig":
                val = orig_b
            elif col == "comp":
                val = comp_b
            elif col == "ratio":
                val = (comp_b / orig_b) if orig_b > 0 else None
            elif col == "savings":
                val = (100.0 - (comp_b / orig_b * 100.0)) if orig_b > 0 else None
            elif col == "vs_base_sz":
                if r.get("id") == self.custom_bench_baseline_id:
                    val = 0.0
                elif base_comp > 0:
                    val = (comp_b - base_comp) / base_comp
            elif col == "enc_time":
                val = r.get("enc_ms")
            elif col == "enc_speed":
                enc_ms = r.get("enc_ms")
                val = (orig_b / (enc_ms / 1000.0)) if (enc_ms and enc_ms > 0 and orig_b > 0) else None
            elif col == "dec_time":
                val = r.get("dec_ms")
            elif col == "dec_speed":
                dec_ms = r.get("dec_ms")
                val = (orig_b / (dec_ms / 1000.0)) if (dec_ms and dec_ms > 0 and orig_b > 0) else None
            if val is None:
                return (1, 0.0)
            return (0, float(val))

        children = list(self.custom_bench_tree.get_children(""))
        children.sort(key=_sort_key, reverse=reverse)
        for index, k in enumerate(children):
            self.custom_bench_tree.move(k, "", index)
        self.custom_bench_tree.heading(col, command=lambda c=col: self.custom_bench_sort_column(c, not reverse))

    def measure_selected_custom_decode(self):
        sel = list(self.custom_bench_tree.selection())
        if not sel:
            messagebox.showinfo("Measure Decode", "Select one or more imported benchmark rows to measure decode speed.")
            return
        rec_by_id = {r["id"]: r for r in self.custom_benchmarks if r.get("id")}
        targets = [rec_by_id[i] for i in sel if i in rec_by_id and rec_by_id[i].get("archive_path") and os.path.exists(rec_by_id[i]["archive_path"])]
        if not targets:
            messagebox.showinfo(
                "Measure Decode",
                "Selected row(s) do not have a persistent .prism archive on disk.\nImport a saved .prism archive to measure decode speed.",
            )
            return

        include_disk_extract = self.qt_disk_extract_var.get() if hasattr(self, "qt_disk_extract_var") else False
        self.cancel_requested = False
        mode_lbl = "Disk Decode" if include_disk_extract else "In-Memory Decode"
        self.start_busy(f"Measuring {mode_lbl} for {len(targets)} archive(s)...")

        def worker():
            measured = 0
            try:
                for idx, rec in enumerate(targets, start=1):
                    if self.cancel_requested:
                        break
                    arc_path = rec["archive_path"]
                    self.ui_queue.put(lambda n=os.path.basename(arc_path), i=idx, tot=len(targets), ml=mode_lbl: self.update_status(left=f"{ml} [{i}/{tot}] {n}..."))
                    if not include_disk_extract:
                        vres = prism.verify_bench(arc_path, compute_sha256=False)
                        if vres["ok"] and vres["dec_ms"] is not None:
                            rec["dec_ms"] = vres["dec_ms"]
                            rec["verified"] = True
                            measured += 1
                        else:
                            rec["verified"] = bool(vres["ok"])
                    else:
                        tmpdir = tempfile.mkdtemp(prefix="prism_dec_")
                        try:
                            is_dir_arc = bool(rec.get("is_dir"))
                            if not is_dir_arc:
                                try:
                                    meta_inf = prism.read_metadata(arc_path)
                                    is_dir_arc = bool(meta_inf.get("is_dir"))
                                except Exception:
                                    pass
                            dec_target = os.path.join(tmpdir, "d") if is_dir_arc else os.path.join(tmpdir, "f")
                            if is_dir_arc:
                                os.makedirs(_win_long_path(dec_target), exist_ok=True)
                            res_v = prism.verify(arc_path, check=False)
                            v_ok = (res_v.returncode == 0)
                            t0 = time.perf_counter()
                            res_d = prism.decompress(arc_path, dec_target, check=False)
                            elapsed_d = time.perf_counter() - t0
                            if res_d.returncode == 0:
                                rec["dec_ms"] = elapsed_d * 1000.0
                                rec["verified"] = v_ok
                                measured += 1
                            else:
                                rec["verified"] = False
                        finally:
                            shutil.rmtree(_win_long_path(tmpdir, force=True), ignore_errors=True)

                def _finish():
                    self.save_custom_benchmarks()
                    self.refresh_custom_benchmarks_ui()
                    self.update_status(left=f"Measured decode for {measured} archive(s)")
                    self.log_activity(f"Measured decode speed for {measured} archive(s).")

                self.ui_queue.put(_finish)
            except Exception as e:
                err_msg = str(e)
                self.ui_queue.put(lambda m=err_msg: messagebox.showerror("Decode Measure Error", m))
            finally:
                self.ui_queue.put(self.stop_busy)

        threading.Thread(target=worker, daemon=True).start()

    def run_quick_test(self, target_type=None):
        if target_type == "folder":
            path = filedialog.askdirectory(title="Select Folder to Quick Test")
        elif target_type == "file":
            path = filedialog.askopenfilename(title="Select File to Quick Test")
        else:
            path = filedialog.askopenfilename(title="Select File to Quick Test")
            if not path:
                path = filedialog.askdirectory(title="Select Folder to Quick Test")
        if not path or not os.path.exists(path):
            return

        qt_mode = self.qt_preset_var.get() if hasattr(self, "qt_preset_var") else "Normal - Default"
        include_disk_extract = self.qt_disk_extract_var.get() if hasattr(self, "qt_disk_extract_var") else False
        decode_mode_note = "Disk Extract" if include_disk_extract else "In-Memory"
        if "Compare Both" in qt_mode:
            presets_to_run = [("Normal", False, None), ("Fast", True, None)]
        elif qt_mode == "Fast":
            presets_to_run = [("Fast", True, None)]
        elif qt_mode.startswith("Ultra (Max)"):
            presets_to_run = [("Ultra", False, None)]
        elif qt_mode.startswith("Level 7"):
            presets_to_run = [("Level 7", False, 7)]
        elif qt_mode.startswith("Level 3"):
            presets_to_run = [("Level 3", False, 3)]
        else:
            presets_to_run = [("Normal", False, None)]

        top = tk.Toplevel(self.root)
        top.title("Quick Test Progress")
        top.geometry("520x260")
        top.configure(bg=self.bg_dark)

        cancelled = False
        lbl = ttk.Label(top, text=f"Running quick test ({qt_mode}) on:\n{path}\n\nInitializing...", font=("Segoe UI", 10))
        lbl.pack(padx=20, pady=20)

        pb = ttk.Progressbar(top, mode="indeterminate", length=300)
        pb.pack(padx=20, pady=10)
        pb.start(10)

        def _on_close():
            nonlocal cancelled
            cancelled = True
            try:
                if top.winfo_exists():
                    top.destroy()
            except Exception:
                pass

        top.protocol("WM_DELETE_WINDOW", _on_close)

        def _set_stage(stage_text):
            if cancelled:
                return
            def _upd(s=stage_text):
                if not cancelled and top.winfo_exists():
                    lbl.config(text=f"Running quick test ({qt_mode}) on:\n{path}\n\n{s}")
            self.ui_queue.put(_upd)

        def worker():
            tmpdir = None
            try:
                _set_stage("Hashing...")
                orig_bytes, _, orig_digest = prism_compute_path_digest(path)
                if cancelled:
                    return
                is_dir_src = os.path.isdir(_win_long_path(path))
                base_label = os.path.basename(path) or path

                tmpdir = tempfile.mkdtemp(prefix="prism_qt_")
                run_records = []
                run_summaries = []

                for preset_name, is_fast, lvl_override in presets_to_run:
                    if cancelled:
                        return
                    tag_p = preset_name.lower()
                    out_arch = os.path.join(tmpdir, f"test_{tag_p}.prism")
                    dec_target = os.path.join(tmpdir, f"d_{tag_p}") if is_dir_src else os.path.join(tmpdir, f"f_{tag_p}")
                    if is_dir_src and include_disk_extract:
                        os.makedirs(_win_long_path(dec_target), exist_ok=True)

                    _set_stage(f"[{preset_name}] Compressing...")
                    start_c = time.perf_counter()
                    res_c = prism.compress(path, out_arch, check=False, fast=is_fast, ultra=(preset_name == "Ultra"), level=lvl_override, embed_meta=True)
                    elapsed_c = time.perf_counter() - start_c
                    if cancelled:
                        return
                    if res_c.returncode != 0:
                        raise RuntimeError(f"[{preset_name}] Compression failed: {res_c.stderr or res_c.stdout}")

                    arch_size = os.path.getsize(out_arch)
                    alg_name = "Standard"
                    created_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                    try:
                        meta_inf = prism.read_metadata(out_arch)
                        alg_name = meta_inf.get("algorithm") or alg_name
                        if meta_inf.get("has_pmet") and isinstance(meta_inf.get("pmet"), dict):
                            created_utc = meta_inf["pmet"].get("created_utc") or created_utc
                    except Exception:
                        pass

                    if not include_disk_extract:
                        _set_stage(f"[{preset_name}] In-Memory Decode & SHA-256...")
                        t_v0 = time.perf_counter()
                        vres = prism.verify_bench(out_arch, compute_sha256=True)
                        wall_d = time.perf_counter() - t_v0
                        if cancelled:
                            return
                        if not vres["ok"]:
                            raise RuntimeError(f"[{preset_name}] Verification failed: {vres['stderr'] or vres['stdout']}")
                        elapsed_d = (vres["dec_ms"] / 1000.0) if (vres["dec_ms"] is not None and vres["dec_ms"] > 0) else wall_d
                        match = bool(vres["sha256"] and vres["sha256"] == orig_digest)
                    else:
                        _set_stage(f"[{preset_name}] Verifying...")
                        res_v = prism.verify(out_arch, check=False)
                        if cancelled:
                            return
                        if res_v.returncode != 0:
                            raise RuntimeError(f"[{preset_name}] Verification failed: {res_v.stderr or res_v.stdout}")

                        _set_stage(f"[{preset_name}] Decompressing...")
                        start_d = time.perf_counter()
                        res_d = prism.decompress(out_arch, dec_target, check=False)
                        elapsed_d = time.perf_counter() - start_d
                        if cancelled:
                            return
                        if res_d.returncode != 0:
                            raise RuntimeError(f"[{preset_name}] Decompression failed: {res_d.stderr or res_d.stdout}")

                        _set_stage(f"[{preset_name}] Checking SHA-256...")
                        _, _, dec_digest = prism_compute_path_digest(dec_target)
                        if cancelled:
                            return
                        match = (dec_digest == orig_digest)

                    rec = self._make_custom_bench_record(
                        label=base_label,
                        source_type="QuickTest",
                        preset=preset_name,
                        algorithm=alg_name,
                        orig_bytes=orig_bytes,
                        comp_bytes=arch_size,
                        enc_ms=elapsed_c * 1000.0,
                        dec_ms=elapsed_d * 1000.0,
                        verified=match,
                        archive_path="",
                        created_utc=created_utc,
                        has_pmet=True,
                        is_dir=is_dir_src,
                    )
                    run_records.append(rec)
                    run_summaries.append((preset_name, arch_size, elapsed_c, elapsed_d, match))

                if len(run_summaries) == 1:
                    preset_name, arch_size, elapsed_c, elapsed_d, match = run_summaries[0]
                    ratio = (arch_size / orig_bytes * 100.0) if orig_bytes > 0 else 0.0
                    msg = (
                        f"Quick Test Complete ({preset_name}, {decode_mode_note})!\n\n"
                        f"Target: {base_label}\n"
                        f"Original Size: {self.format_size(orig_bytes)}\n"
                        f"Archive Size: {self.format_size(arch_size)} ({ratio:.2f}%)\n"
                        f"Encode Speed: {self.format_speed_from_bytes(orig_bytes, elapsed_c)} ({elapsed_c:.2f}s)\n"
                        f"Decode Speed ({decode_mode_note}): {self.format_speed_from_bytes(orig_bytes, elapsed_d)} ({elapsed_d:.2f}s)\n"
                        f"SHA-256 Match: {'MATCH (Verified)' if match else 'MISMATCH'}"
                    )
                else:
                    u_name, u_sz, u_ec, u_ed, u_ok = run_summaries[0]
                    f_name, f_sz, f_ec, f_ed, f_ok = run_summaries[1]
                    u_ratio = (u_sz / orig_bytes * 100.0) if orig_bytes > 0 else 0.0
                    f_ratio = (f_sz / orig_bytes * 100.0) if orig_bytes > 0 else 0.0
                    diff_b = f_sz - u_sz
                    diff_sign = "-" if diff_b < 0 else "+"
                    diff_pct = ((f_sz - u_sz) / u_sz * 100.0) if u_sz > 0 else 0.0
                    spd_mult = (u_ec / f_ec) if f_ec > 0 else 0.0
                    msg = (
                        f"Quick Test Comparison Complete (Normal vs Fast, {decode_mode_note})!\n\n"
                        f"Target: {base_label} ({self.format_size(orig_bytes)})\n\n"
                        f"[{u_name}] {self.format_size(u_sz)} ({u_ratio:.2f}%) | "
                        f"Enc {self.format_speed_from_bytes(orig_bytes, u_ec)} | "
                        f"Dec {self.format_speed_from_bytes(orig_bytes, u_ed)} | "
                        f"SHA-256: {'MATCH' if u_ok else 'MISMATCH'}\n"
                        f"[{f_name}] {self.format_size(f_sz)} ({f_ratio:.2f}%) | "
                        f"Enc {self.format_speed_from_bytes(orig_bytes, f_ec)} | "
                        f"Dec {self.format_speed_from_bytes(orig_bytes, f_ed)} | "
                        f"SHA-256: {'MATCH' if f_ok else 'MISMATCH'}\n\n"
                        f"Fast vs Normal: {diff_sign}{self.format_size(abs(diff_b))} ({diff_pct:+.2f}%) | Encode Speedup: {spd_mult:.2f}x"
                    )

                def _done(m=msg, recs=run_records):
                    if cancelled:
                        return
                    self.custom_benchmarks.extend(recs)
                    self.save_custom_benchmarks()
                    self.refresh_custom_benchmarks_ui()
                    new_ids = [r["id"] for r in recs if self.custom_bench_tree.exists(r["id"])]
                    if new_ids:
                        self.custom_bench_tree.selection_set(new_ids)
                        self.update_custom_benchmark_summary()
                        self.draw_custom_benchmark_chart()
                    if hasattr(self, "bench_sub_notebook") and hasattr(self, "tab_bench_custom"):
                        self.notebook.select(self.tab_benchmark)
                        self.bench_sub_notebook.select(self.tab_bench_custom)
                    if top.winfo_exists():
                        top.destroy()
                    messagebox.showinfo("Quick Test Results", m)

                self.ui_queue.put(_done)
            except Exception as e:
                if not cancelled:
                    err_msg = str(e)
                    def _err(m=err_msg):
                        if cancelled:
                            return
                        if top.winfo_exists():
                            top.destroy()
                        messagebox.showerror("Quick Test Error", m)
                    self.ui_queue.put(_err)
            finally:
                if tmpdir:
                    shutil.rmtree(_win_long_path(tmpdir, force=True), ignore_errors=True)

        threading.Thread(target=worker, daemon=True).start()

def _win_long_path(p: str, force: bool = False) -> str:
    if os.name != "nt" or not p:
        return p
    if p.startswith("\\\\?\\"):
        return p
    ap = os.path.abspath(p)
    if force or len(ap) >= 240:
        if ap.startswith("\\\\"):
            return "\\\\?\\UNC\\" + ap[2:]
        return "\\\\?\\" + ap
    return ap

def prism_compute_path_digest(path_str):
    lp = _win_long_path(str(path_str))
    if os.path.isfile(lp):
        h = hashlib.sha256()
        size = os.path.getsize(lp)
        with open(lp, "rb") as f:
            while chunk := f.read(65536):
                h.update(chunk)
        return size, 1, h.hexdigest()
    elif os.path.isdir(lp):
        h = hashlib.sha256()
        total_size = 0
        file_count = 0
        walk_root = _win_long_path(str(path_str), force=True)
        entries = []
        for dirpath, _dirnames, filenames in os.walk(walk_root):
            for fn in filenames:
                full_p = os.path.join(dirpath, fn)
                rel = os.path.relpath(full_p, walk_root).replace("\\", "/")
                entries.append((rel, full_p))
        entries.sort(key=lambda item: item[0])
        for rel, full_p in entries:
            fp = _win_long_path(full_p)
            f_size = os.path.getsize(fp)
            total_size += f_size
            file_count += 1
            h.update(rel.encode("utf-8"))
            h.update(str(f_size).encode("utf-8"))
            with open(fp, "rb") as rf:
                while chunk := rf.read(65536):
                    h.update(chunk)
        return total_size, file_count, h.hexdigest()
    return 0, 0, ""

def main():
    root = tk.Tk()
    app = PrismArchiverApp(root)
    root.mainloop()

if __name__ == "__main__":
    main()
