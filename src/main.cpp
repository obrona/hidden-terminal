// hidden-terminal — a full-featured terminal emulator (GTK4 + VTE).
//
// The window uses the fixed Wayland app-id "dev.hidden.Terminal". Under the
// niri compositor a window rule matching that app-id removes it from every
// screenshot / screencast (`block-out-from "screen-capture" kind="hide"`),
// so captures show whatever is behind it. See README.md.

#define PCRE2_CODE_UNIT_WIDTH 0
#include <pcre2.h>

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <string>
#include <vector>

static constexpr const char *APP_ID = "dev.hidden.Terminal";
static constexpr guint32 RE_FLAGS = PCRE2_UTF | PCRE2_NO_UTF_CHECK | PCRE2_MULTILINE;

// Command-line options (apply to the first window only).
static std::vector<std::string> g_exec_argv;   // -e / -- command
static std::string g_workdir;                  // -w DIR

struct Win {
    GtkApplicationWindow *window;
    GtkNotebook *notebook;
    GtkSearchBar *search_bar;
    GtkSearchEntry *search_entry;
};

// ---------------------------------------------------------------------------
// Colours: a dark Tango-style palette.
static const char *PALETTE_HEX[16] = {
    "#2e3436", "#cc0000", "#4e9a06", "#c4a000", "#3465a4", "#75507b", "#06989a", "#d3d7cf",
    "#555753", "#ef2929", "#8ae234", "#fce94f", "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec",
};

static const char *URL_REGEX =
    "(?:https?|ftp|file)://[\\w\\-.~:/?#\\[\\]@!$&'()*+,;=%]+[\\w\\-~/#=&+%]";

// ---------------------------------------------------------------------------
// Helpers

static VteTerminal *page_terminal(GtkWidget *page) {
    return page ? VTE_TERMINAL(g_object_get_data(G_OBJECT(page), "term")) : nullptr;
}

static VteTerminal *current_terminal(Win *w) {
    int n = gtk_notebook_get_current_page(w->notebook);
    if (n < 0) return nullptr;
    return page_terminal(gtk_notebook_get_nth_page(w->notebook, n));
}

static GtkWidget *terminal_page(VteTerminal *term) {
    return GTK_WIDGET(g_object_get_data(G_OBJECT(term), "page"));
}

static std::string terminal_title(VteTerminal *term) {
    size_t len = 0;
    const char *t = vte_terminal_get_termprop_string(term, VTE_TERMPROP_XTERM_TITLE, &len);
    if (t && len) return std::string(t, len);
    return "Terminal";
}

static std::string terminal_cwd(VteTerminal *term) {
    std::string out;
    if (!term) return out;
    GUri *uri = vte_terminal_ref_termprop_uri(term, VTE_TERMPROP_CURRENT_DIRECTORY_URI);
    if (uri) {
        char *s = g_uri_to_string(uri);
        char *path = g_filename_from_uri(s, nullptr, nullptr);
        if (path) out = path;
        g_free(path);
        g_free(s);
        g_uri_unref(uri);
    }
    return out;
}

static void update_titles(Win *w) {
    int n = gtk_notebook_get_n_pages(w->notebook);
    gtk_notebook_set_show_tabs(w->notebook, n > 1);
    VteTerminal *cur = current_terminal(w);
    if (cur) gtk_window_set_title(GTK_WINDOW(w->window), terminal_title(cur).c_str());
}

static void launch_uri(GtkWidget *from, const char *uri) {
    GtkUriLauncher *l = gtk_uri_launcher_new(uri);
    GtkRoot *root = gtk_widget_get_root(from);
    gtk_uri_launcher_launch(l, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr, nullptr, nullptr,
                            nullptr);
    g_object_unref(l);
}

static Win *new_window(GtkApplication *app, const std::string &cwd,
                       const std::vector<std::string> &cmd);
static void add_tab(Win *w, const std::string &cwd, const std::vector<std::string> &cmd);

// ---------------------------------------------------------------------------
// Terminal signal handlers

static void on_child_exited(VteTerminal *term, int, gpointer data) {
    Win *w = static_cast<Win *>(data);
    GtkWidget *page = terminal_page(term);
    int idx = gtk_notebook_page_num(w->notebook, page);
    if (idx >= 0) gtk_notebook_remove_page(w->notebook, idx);
    if (gtk_notebook_get_n_pages(w->notebook) == 0)
        gtk_window_destroy(GTK_WINDOW(w->window));
    else
        update_titles(w);
}

static void on_termprop_changed(VteTerminal *term, const char *, gpointer data) {
    Win *w = static_cast<Win *>(data);
    GtkLabel *label = GTK_LABEL(g_object_get_data(G_OBJECT(term), "label"));
    std::string t = terminal_title(term);
    if (label) gtk_label_set_text(label, t.c_str());
    if (term == current_terminal(w)) gtk_window_set_title(GTK_WINDOW(w->window), t.c_str());
}

static void on_spawned(VteTerminal *term, GPid, GError *error, gpointer) {
    if (error) {
        std::string msg = std::string("\r\nFailed to start: ") + error->message + "\r\n";
        vte_terminal_feed(term, msg.c_str(), (gssize)msg.size());
    }
}

// Ctrl+click opens URLs / OSC 8 hyperlinks.
static void on_term_click(GtkGestureClick *g, int, double x, double y, gpointer) {
    GtkWidget *wid = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    VteTerminal *term = VTE_TERMINAL(wid);
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (!(mods & GDK_CONTROL_MASK)) return;

    char *link = vte_terminal_check_hyperlink_at(term, x, y);
    if (!link) {
        int tag = 0;
        link = vte_terminal_check_match_at(term, x, y, &tag);
    }
    if (link) {
        launch_uri(wid, link);
        g_free(link);
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

// Right-click context menu.
static void on_term_right_click(GtkGestureClick *g, int, double x, double y, gpointer) {
    GtkWidget *wid = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkPopover *pop = GTK_POPOVER(g_object_get_data(G_OBJECT(wid), "menu"));
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(pop, &r);
    gtk_popover_popup(pop);
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void on_term_destroy(GtkWidget *term, gpointer) {
    GtkWidget *pop = GTK_WIDGET(g_object_get_data(G_OBJECT(term), "menu"));
    if (pop) {
        g_object_set_data(G_OBJECT(term), "menu", nullptr);
        gtk_widget_unparent(pop);
    }
}

static GMenuModel *build_context_menu() {
    GMenu *m = g_menu_new();
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, "Copy", "win.copy");
    g_menu_append(s1, "Paste", "win.paste");
    g_menu_append(s1, "Select All", "win.select-all");
    g_menu_append_section(m, nullptr, G_MENU_MODEL(s1));
    GMenu *s2 = g_menu_new();
    g_menu_append(s2, "New Tab", "win.new-tab");
    g_menu_append(s2, "New Window", "app.new-window");
    g_menu_append(s2, "Find…", "win.find");
    g_menu_append_section(m, nullptr, G_MENU_MODEL(s2));
    GMenu *s3 = g_menu_new();
    g_menu_append(s3, "Zoom In", "win.zoom-in");
    g_menu_append(s3, "Zoom Out", "win.zoom-out");
    g_menu_append(s3, "Normal Size", "win.zoom-reset");
    g_menu_append(s3, "Full Screen", "win.fullscreen");
    g_menu_append_section(m, nullptr, G_MENU_MODEL(s3));
    GMenu *s4 = g_menu_new();
    g_menu_append(s4, "Close Tab", "win.close-tab");
    g_menu_append_section(m, nullptr, G_MENU_MODEL(s4));
    g_object_unref(s1);
    g_object_unref(s2);
    g_object_unref(s3);
    g_object_unref(s4);
    return G_MENU_MODEL(m);
}

static void configure_terminal(VteTerminal *term) {
    PangoFontDescription *font = pango_font_description_from_string("Monospace 11");
    vte_terminal_set_font(term, font);
    pango_font_description_free(font);

    GdkRGBA palette[16], fg, bg;
    for (int i = 0; i < 16; i++) gdk_rgba_parse(&palette[i], PALETTE_HEX[i]);
    gdk_rgba_parse(&fg, "#d0d0d0");
    gdk_rgba_parse(&bg, "#1e1e1e");
    vte_terminal_set_colors(term, &fg, &bg, palette, 16);

    vte_terminal_set_scrollback_lines(term, 10000);
    vte_terminal_set_scroll_on_output(term, FALSE);
    vte_terminal_set_scroll_on_keystroke(term, TRUE);
    vte_terminal_set_mouse_autohide(term, TRUE);
    vte_terminal_set_allow_hyperlink(term, TRUE);
    vte_terminal_set_audible_bell(term, TRUE);
    vte_terminal_set_bold_is_bright(term, FALSE);
    vte_terminal_set_cursor_blink_mode(term, VTE_CURSOR_BLINK_SYSTEM);
    vte_terminal_set_enable_fallback_scrolling(term, TRUE);

    GError *err = nullptr;
    VteRegex *re = vte_regex_new_for_match(URL_REGEX, -1, RE_FLAGS, &err);
    if (re) {
        int tag = vte_terminal_match_add_regex(term, re, 0);
        vte_terminal_match_set_cursor_name(term, tag, "pointer");
        vte_regex_unref(re);
    } else {
        g_warning("URL regex: %s", err->message);
        g_clear_error(&err);
    }
}

// ---------------------------------------------------------------------------
// Tabs

static void on_tab_close_clicked(GtkButton *, gpointer data) {
    VteTerminal *term = VTE_TERMINAL(data);
    Win *w = static_cast<Win *>(g_object_get_data(G_OBJECT(term), "win"));
    // Treat like the child exiting (SIGHUP is delivered when the PTY closes).
    on_child_exited(term, 0, w);
}

static void add_tab(Win *w, const std::string &cwd_in, const std::vector<std::string> &cmd) {
    GtkWidget *term_w = vte_terminal_new();
    VteTerminal *term = VTE_TERMINAL(term_w);
    configure_terminal(term);
    gtk_widget_set_hexpand(term_w, TRUE);
    gtk_widget_set_vexpand(term_w, TRUE);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(page), term_w);
    GtkWidget *sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
                                      gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(term)));
    gtk_box_append(GTK_BOX(page), sb);

    // Tab label with close button.
    GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *label = gtk_label_new("Terminal");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 12);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
    gtk_widget_set_hexpand(label, TRUE);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_focus_on_click(close, FALSE);
    gtk_box_append(GTK_BOX(tab), label);
    gtk_box_append(GTK_BOX(tab), close);
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), term);

    g_object_set_data(G_OBJECT(page), "term", term);
    g_object_set_data(G_OBJECT(term), "page", page);
    g_object_set_data(G_OBJECT(term), "label", label);
    g_object_set_data(G_OBJECT(term), "win", w);

    // Context menu.
    GMenuModel *model = build_context_menu();
    GtkWidget *pop = gtk_popover_menu_new_from_model(model);
    g_object_unref(model);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    gtk_widget_set_parent(pop, term_w);
    g_object_set_data(G_OBJECT(term), "menu", pop);
    g_signal_connect(term_w, "destroy", G_CALLBACK(on_term_destroy), nullptr);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_term_click), nullptr);
    gtk_widget_add_controller(term_w, GTK_EVENT_CONTROLLER(click));

    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rclick), GTK_PHASE_CAPTURE);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_term_right_click), nullptr);
    gtk_widget_add_controller(term_w, GTK_EVENT_CONTROLLER(rclick));

    g_signal_connect(term, "child-exited", G_CALLBACK(on_child_exited), w);
    g_signal_connect(term, "termprop-changed::" VTE_TERMPROP_XTERM_TITLE,
                     G_CALLBACK(on_termprop_changed), w);
    g_signal_connect(term, "bell", G_CALLBACK(+[](VteTerminal *t, gpointer) {
                         gtk_widget_error_bell(GTK_WIDGET(t));
                     }), nullptr);

    int idx = gtk_notebook_append_page(w->notebook, page, tab);
    gtk_notebook_set_tab_reorderable(w->notebook, page, TRUE);
    gtk_notebook_set_tab_detachable(w->notebook, page, FALSE);
    gtk_notebook_set_current_page(w->notebook, idx);
    update_titles(w);

    // Spawn the shell (or the requested command).
    std::vector<std::string> args = cmd;
    if (args.empty()) {
        const char *shell = g_getenv("SHELL");
        if (!shell || !*shell || !g_file_test(shell, G_FILE_TEST_IS_EXECUTABLE)) {
            char *s = vte_get_user_shell();
            args.push_back(s && *s ? s : "/bin/bash");
            g_free(s);
        } else {
            args.push_back(shell);
        }
    }
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    std::string cwd = cwd_in;
    if (cwd.empty() || !g_file_test(cwd.c_str(), G_FILE_TEST_IS_DIR)) cwd = g_get_home_dir();

    vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, cwd.c_str(), argv.data(), nullptr,
                             G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, -1, nullptr,
                             on_spawned, nullptr);
    gtk_widget_grab_focus(term_w);
}

// ---------------------------------------------------------------------------
// Search

static void do_search(Win *w, bool backwards) {
    VteTerminal *term = current_terminal(w);
    if (!term) return;
    if (backwards)
        vte_terminal_search_find_previous(term);
    else
        vte_terminal_search_find_next(term);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    Win *w = static_cast<Win *>(data);
    VteTerminal *term = current_terminal(w);
    if (!term) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!text || !*text) {
        vte_terminal_search_set_regex(term, nullptr, 0);
        vte_terminal_unselect_all(term);
        return;
    }
    char *escaped = g_regex_escape_string(text, -1);
    guint32 flags = RE_FLAGS;
    // Smart case: case-insensitive unless the query has an uppercase letter.
    bool has_upper = false;
    for (const char *p = text; *p; p = g_utf8_next_char(p))
        if (g_unichar_isupper(g_utf8_get_char(p))) has_upper = true;
    if (!has_upper) flags |= PCRE2_CASELESS;
    GError *err = nullptr;
    VteRegex *re = vte_regex_new_for_search(escaped, -1, flags, &err);
    g_free(escaped);
    if (!re) {
        g_clear_error(&err);
        return;
    }
    vte_terminal_search_set_regex(term, re, 0);
    vte_terminal_search_set_wrap_around(term, TRUE);
    vte_regex_unref(re);
    vte_terminal_search_find_previous(term);  // search upward from the bottom
}

static void on_search_activate(GtkSearchEntry *, gpointer data) {
    do_search(static_cast<Win *>(data), true);
}
static void on_search_next(GtkSearchEntry *, gpointer data) {
    do_search(static_cast<Win *>(data), false);
}
static void on_search_prev(GtkSearchEntry *, gpointer data) {
    do_search(static_cast<Win *>(data), true);
}
static void on_search_stop(GtkSearchEntry *, gpointer data) {
    Win *w = static_cast<Win *>(data);
    gtk_search_bar_set_search_mode(w->search_bar, FALSE);
    if (VteTerminal *t = current_terminal(w)) gtk_widget_grab_focus(GTK_WIDGET(t));
}

// ---------------------------------------------------------------------------
// Window actions

#define WIN_ACTION(name) static void name(GSimpleAction *, GVariant *, gpointer data)

WIN_ACTION(act_new_tab) {
    Win *w = static_cast<Win *>(data);
    add_tab(w, terminal_cwd(current_terminal(w)), {});
}
WIN_ACTION(act_close_tab) {
    Win *w = static_cast<Win *>(data);
    if (VteTerminal *t = current_terminal(w)) on_child_exited(t, 0, w);
}
WIN_ACTION(act_close_window) { gtk_window_close(GTK_WINDOW(static_cast<Win *>(data)->window)); }
WIN_ACTION(act_copy) {
    if (VteTerminal *t = current_terminal(static_cast<Win *>(data)))
        if (vte_terminal_get_has_selection(t)) vte_terminal_copy_clipboard_format(t, VTE_FORMAT_TEXT);
}
WIN_ACTION(act_paste) {
    if (VteTerminal *t = current_terminal(static_cast<Win *>(data))) vte_terminal_paste_clipboard(t);
}
WIN_ACTION(act_select_all) {
    if (VteTerminal *t = current_terminal(static_cast<Win *>(data))) vte_terminal_select_all(t);
}
WIN_ACTION(act_next_tab) {
    Win *w = static_cast<Win *>(data);
    int n = gtk_notebook_get_n_pages(w->notebook);
    gtk_notebook_set_current_page(w->notebook, (gtk_notebook_get_current_page(w->notebook) + 1) % n);
}
WIN_ACTION(act_prev_tab) {
    Win *w = static_cast<Win *>(data);
    int n = gtk_notebook_get_n_pages(w->notebook);
    gtk_notebook_set_current_page(w->notebook, (gtk_notebook_get_current_page(w->notebook) + n - 1) % n);
}
static void move_tab(Win *w, int delta) {
    int n = gtk_notebook_get_n_pages(w->notebook);
    int cur = gtk_notebook_get_current_page(w->notebook);
    int to = cur + delta;
    if (to < 0 || to >= n) return;
    gtk_notebook_reorder_child(w->notebook, gtk_notebook_get_nth_page(w->notebook, cur), to);
}
WIN_ACTION(act_move_tab_left) { move_tab(static_cast<Win *>(data), -1); }
WIN_ACTION(act_move_tab_right) { move_tab(static_cast<Win *>(data), +1); }
static void act_goto_tab(GSimpleAction *, GVariant *param, gpointer data) {
    Win *w = static_cast<Win *>(data);
    int i = g_variant_get_int32(param);
    int n = gtk_notebook_get_n_pages(w->notebook);
    gtk_notebook_set_current_page(w->notebook, i <= 0 || i > n ? n - 1 : i - 1);
}
static void zoom(Win *w, double factor) {
    VteTerminal *t = current_terminal(w);
    if (!t) return;
    double s = factor == 0 ? 1.0 : vte_terminal_get_font_scale(t) * factor;
    vte_terminal_set_font_scale(t, CLAMP(s, 0.25, 4.0));
}
WIN_ACTION(act_zoom_in) { zoom(static_cast<Win *>(data), 1.1); }
WIN_ACTION(act_zoom_out) { zoom(static_cast<Win *>(data), 1 / 1.1); }
WIN_ACTION(act_zoom_reset) { zoom(static_cast<Win *>(data), 0); }
WIN_ACTION(act_find) {
    Win *w = static_cast<Win *>(data);
    gtk_search_bar_set_search_mode(w->search_bar, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(w->search_entry));
}
WIN_ACTION(act_find_next) { do_search(static_cast<Win *>(data), false); }
WIN_ACTION(act_find_prev) { do_search(static_cast<Win *>(data), true); }
WIN_ACTION(act_fullscreen) {
    GtkWindow *win = GTK_WINDOW(static_cast<Win *>(data)->window);
    if (gtk_window_is_fullscreen(win))
        gtk_window_unfullscreen(win);
    else
        gtk_window_fullscreen(win);
}
WIN_ACTION(act_reset) {
    if (VteTerminal *t = current_terminal(static_cast<Win *>(data))) vte_terminal_reset(t, TRUE, TRUE);
}

static void on_switch_page(GtkNotebook *, GtkWidget *page, guint, gpointer data) {
    Win *w = static_cast<Win *>(data);
    if (VteTerminal *t = page_terminal(page)) {
        gtk_window_set_title(GTK_WINDOW(w->window), terminal_title(t).c_str());
        gtk_widget_grab_focus(GTK_WIDGET(t));
    }
}

static void on_window_destroy(GtkWidget *, gpointer data) { delete static_cast<Win *>(data); }

static Win *new_window(GtkApplication *app, const std::string &cwd,
                       const std::vector<std::string> &cmd) {
    Win *w = new Win();
    w->window = GTK_APPLICATION_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(w->window), "Terminal");
    gtk_window_set_default_size(GTK_WINDOW(w->window), 900, 560);
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "utilities-terminal");

    static const GActionEntry entries[] = {
        {"new-tab", act_new_tab, nullptr, nullptr, nullptr, {}},
        {"close-tab", act_close_tab, nullptr, nullptr, nullptr, {}},
        {"close-window", act_close_window, nullptr, nullptr, nullptr, {}},
        {"copy", act_copy, nullptr, nullptr, nullptr, {}},
        {"paste", act_paste, nullptr, nullptr, nullptr, {}},
        {"select-all", act_select_all, nullptr, nullptr, nullptr, {}},
        {"next-tab", act_next_tab, nullptr, nullptr, nullptr, {}},
        {"prev-tab", act_prev_tab, nullptr, nullptr, nullptr, {}},
        {"move-tab-left", act_move_tab_left, nullptr, nullptr, nullptr, {}},
        {"move-tab-right", act_move_tab_right, nullptr, nullptr, nullptr, {}},
        {"goto-tab", act_goto_tab, "i", nullptr, nullptr, {}},
        {"zoom-in", act_zoom_in, nullptr, nullptr, nullptr, {}},
        {"zoom-out", act_zoom_out, nullptr, nullptr, nullptr, {}},
        {"zoom-reset", act_zoom_reset, nullptr, nullptr, nullptr, {}},
        {"find", act_find, nullptr, nullptr, nullptr, {}},
        {"find-next", act_find_next, nullptr, nullptr, nullptr, {}},
        {"find-prev", act_find_prev, nullptr, nullptr, nullptr, {}},
        {"fullscreen", act_fullscreen, nullptr, nullptr, nullptr, {}},
        {"reset", act_reset, nullptr, nullptr, nullptr, {}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(w->window), entries, G_N_ELEMENTS(entries), w);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Search bar.
    w->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->search_entry), 320, -1);
    w->search_bar = GTK_SEARCH_BAR(gtk_search_bar_new());
    gtk_search_bar_set_child(w->search_bar, GTK_WIDGET(w->search_entry));
    gtk_search_bar_set_show_close_button(w->search_bar, TRUE);
    gtk_search_bar_connect_entry(w->search_bar, GTK_EDITABLE(w->search_entry));
    g_signal_connect(w->search_entry, "search-changed", G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->search_entry, "activate", G_CALLBACK(on_search_activate), w);
    g_signal_connect(w->search_entry, "next-match", G_CALLBACK(on_search_next), w);
    g_signal_connect(w->search_entry, "previous-match", G_CALLBACK(on_search_prev), w);
    g_signal_connect(w->search_entry, "stop-search", G_CALLBACK(on_search_stop), w);
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(w->search_bar));

    w->notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_scrollable(w->notebook, TRUE);
    gtk_notebook_set_show_border(w->notebook, FALSE);
    gtk_widget_set_vexpand(GTK_WIDGET(w->notebook), TRUE);
    g_signal_connect(w->notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(w->notebook));

    gtk_window_set_child(GTK_WINDOW(w->window), vbox);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), w);

    add_tab(w, cwd, cmd);
    gtk_window_present(GTK_WINDOW(w->window));
    return w;
}

// ---------------------------------------------------------------------------
// Application

static void app_new_window(GSimpleAction *, GVariant *, gpointer app) {
    GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(app));
    std::string cwd;
    if (active) {
        // Inherit the cwd of the active window's current tab if we can find it.
        GtkWidget *child = gtk_window_get_child(active);
        if (child) {
            GtkWidget *nb = gtk_widget_get_last_child(child);
            if (GTK_IS_NOTEBOOK(nb)) {
                int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
                if (n >= 0) cwd = terminal_cwd(page_terminal(gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), n)));
            }
        }
    }
    new_window(GTK_APPLICATION(app), cwd, {});
}

static void on_activate(GtkApplication *app, gpointer) {
    static bool first = true;
    if (first) {
        first = false;
        new_window(app, g_workdir, g_exec_argv);
    } else {
        new_window(app, "", {});
    }
}

static void on_startup(GtkApplication *app, gpointer) {
    static const GActionEntry app_entries[] = {
        {"new-window", app_new_window, nullptr, nullptr, nullptr, {}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);

    struct { const char *action; std::vector<const char *> keys; } accels[] = {
        {"app.new-window", {"<Control><Shift>n"}},
        {"win.new-tab", {"<Control><Shift>t"}},
        {"win.close-tab", {"<Control><Shift>w"}},
        {"win.close-window", {"<Control><Shift>q"}},
        {"win.copy", {"<Control><Shift>c", "<Control>Insert"}},
        {"win.paste", {"<Control><Shift>v", "<Shift>Insert"}},
        {"win.select-all", {"<Control><Shift>a"}},
        {"win.next-tab", {"<Control>Page_Down"}},
        {"win.prev-tab", {"<Control>Page_Up"}},
        {"win.move-tab-left", {"<Control><Shift>Page_Up"}},
        {"win.move-tab-right", {"<Control><Shift>Page_Down"}},
        {"win.zoom-in", {"<Control>plus", "<Control>equal", "<Control>KP_Add"}},
        {"win.zoom-out", {"<Control>minus", "<Control>KP_Subtract"}},
        {"win.zoom-reset", {"<Control>0", "<Control>KP_0"}},
        {"win.find", {"<Control><Shift>f"}},
        {"win.find-next", {"<Control><Shift>g"}},
        {"win.find-prev", {"<Control><Shift>h"}},
        {"win.fullscreen", {"F11"}},
        {"win.reset", {"<Control><Shift>k"}},
    };
    for (auto &a : accels) {
        std::vector<const char *> k = a.keys;
        k.push_back(nullptr);
        gtk_application_set_accels_for_action(app, a.action, k.data());
    }
    for (int i = 1; i <= 9; i++) {
        char action[32], key[16];
        g_snprintf(action, sizeof action, "win.goto-tab(%d)", i == 9 ? 0 : i);
        g_snprintf(key, sizeof key, "<Alt>%d", i);
        const char *keys[] = {key, nullptr};
        gtk_application_set_accels_for_action(app, action, keys);
    }

    GtkSettings *s = gtk_settings_get_default();
    g_object_set(s, "gtk-application-prefer-dark-theme", TRUE, nullptr);
}

static void usage(const char *prog) {
    g_print("Usage: %s [-w DIR] [-e COMMAND [ARGS...]] [-- COMMAND [ARGS...]]\n", prog);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if ((a == "-w" || a == "--working-directory") && i + 1 < argc) {
            g_workdir = argv[++i];
        } else if (a == "-e" || a == "-x" || a == "--") {
            for (int j = i + 1; j < argc; j++) g_exec_argv.push_back(argv[j]);
            if (a == "-e" && g_exec_argv.size() == 1) {
                // `-e "cmd with args"` form: run through the shell.
                std::string c = g_exec_argv[0];
                g_exec_argv = {"/bin/sh", "-c", c};
            }
            break;
        } else {
            g_printerr("Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    // NON_UNIQUE: each launch is its own process, so -e/-w always apply.
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    char *run_argv[] = {argv[0], nullptr};
    int status = g_application_run(G_APPLICATION(app), 1, run_argv);
    g_object_unref(app);
    return status;
}
