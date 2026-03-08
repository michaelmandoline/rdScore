// rdScore.cpp (v1.1.1 candidate - menu, open/print, basic setlist dialog, single-field extract)
// Build:
//   g++ -O2 -std=c++17 rdScore.cpp -o rdScore $(pkg-config --cflags --libs gtk+-3.0 poppler-glib)

#include <gtk/gtk.h>
#include <poppler.h>

#include <algorithm>
#include <string>
#include <cmath>
#include <vector>
#include <fstream>
#include <sstream>
#include <cctype>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>

static std::string get_setlists_directory() {
  const char* home = getenv("HOME");
  if (!home) return std::string();
  return std::string(home) + "/.local/share/rdscore/setlists";
}

static void ensure_setlists_directory() {
  const char* home = getenv("HOME");
  if (!home) return;

  std::string base = std::string(home) + "/.local/share/rdscore";
  std::string dir  = base + "/setlists";

  mkdir(base.c_str(), 0755);
  mkdir(dir.c_str(), 0755);
}

static std::string trim_copy(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

static std::string basename_only(const std::string& path) {
  char* b = g_path_get_basename(path.c_str());
  std::string out = b ? b : path;
  if (b) g_free(b);
  return out;
}

static const char* RDSCORE_VERSION = "1.1.1";

struct AppState {
  PopplerDocument* doc = nullptr;
  int n_pages = 0;
  int current_left = 0;

  bool two_pages = true;
  bool fullscreen = true; 
  bool pending_recenter = false; // request centering after a size change (e.g., leaving fullscreen)
// concert default
  double zoom = 1.0;      // 1.0 = 100%
  int zoom_percent = 100; // cached for help/overlay

  // Zoom overlay (B)
  bool zoom_overlay = false;
  int zoom_overlay_percent = 100;
  guint zoom_overlay_timer = 0;

  // Page overlay (after page change)
  bool page_overlay = false;
  std::string page_overlay_text;
  guint page_overlay_timer = 0;

  // For extraction (E)
  std::string input_pdf_abs; // absolute path to source PDF

  // Setlist context / chooser memory
  std::string active_setlist_path;
  bool current_doc_from_setlist = false;
  std::string last_pdf_dir;

  GtkWidget* window = nullptr;
  GtkWidget* scrolled = nullptr;
  GtkWidget* drawing = nullptr;
  GtkWidget* menubar = nullptr;
  GtkWidget* menu_file = nullptr;
  GtkWidget* menu_setlists = nullptr;
  GtkWidget* menu_help = nullptr;
  GtkWidget* status_label = nullptr;

  // last computed content size (for size_request)
  int contentW = 1200;
  int contentH = 800;
};

static inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }
static inline double clampd(double v, double lo, double hi) { return std::max(lo, std::min(v, hi)); }

static inline void update_zoom_percent(AppState* s) {
  if (!s) return;
  s->zoom = clampd(s->zoom, 0.30, 5.00);
  s->zoom_percent = (int)std::lround(s->zoom * 100.0);
}

static void update_status_label(AppState* s) {
  if (!s || !s->status_label || !GTK_IS_LABEL(s->status_label)) return;

  std::string text;
  if (!s->doc || s->n_pages <= 0) {
    text = "No PDF | Zoom " + std::to_string(s->zoom_percent) + "%";
  } else if (s->two_pages) {
    const int left = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
    const int right = (left + 1 < s->n_pages) ? (left + 1) : -1;
    if (right >= 0)
      text = "Pages " + std::to_string(left + 1) + "-" + std::to_string(right + 1) +
             " / " + std::to_string(s->n_pages) + " | Zoom " + std::to_string(s->zoom_percent) + "%";
    else
      text = "Page " + std::to_string(left + 1) +
             " / " + std::to_string(s->n_pages) + " | Zoom " + std::to_string(s->zoom_percent) + "%";
  } else {
    const int page = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
    text = "Page " + std::to_string(page + 1) +
           " / " + std::to_string(s->n_pages) + " | Zoom " + std::to_string(s->zoom_percent) + "%";
  }

  gtk_label_set_text(GTK_LABEL(s->status_label), text.c_str());
}

static void normalize_left(AppState* s) {
  s->current_left = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
  if (s->two_pages && (s->current_left % 2 == 1)) s->current_left--;
  s->current_left = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
}

static void hide_cursor(GtkWidget* widget) {
  (void)widget; // keep pointer visible while menu is available
}

static void show_cursor(GtkWidget* widget) {
  GdkWindow* gw = gtk_widget_get_window(widget);
  if (!gw) return;
  gdk_window_set_cursor(gw, nullptr);
}

static void restore_focus(AppState* s) {
  if (!s) return;
  if (s->drawing) gtk_widget_grab_focus(s->drawing);
}

// Ensure mouse cursor is visible while a dialog is open (fixes "blank cursor" when fullscreen hide_cursor is active)
static void dialog_begin(AppState* s, GtkWidget* dialog) {
  if (!s) return;
  // If we're in fullscreen, the main window uses a blank cursor. Temporarily restore it so dialogs show a normal pointer
  if (s->fullscreen) show_cursor(s->window);

  if (dialog) {
    // Realize so it owns a GdkWindow, then ensure default cursor
    gtk_widget_realize(dialog);
    show_cursor(dialog);
  }
}

static void dialog_end(AppState* s) {
  if (!s) return;
  restore_focus(s);
  show_cursor(s->window);
}

static void toggle_fullscreen(AppState* s) {
  s->fullscreen = !s->fullscreen;
  if (s->fullscreen) {
    gtk_window_fullscreen(GTK_WINDOW(s->window));
    show_cursor(s->window);
  } else {
    gtk_window_unfullscreen(GTK_WINDOW(s->window));
    show_cursor(s->window);
    // When leaving fullscreen, GTK may keep the previous scroll offset until the next
    // size allocation / redraw. Request a recenter so the content is immediately placed.
    s->pending_recenter = true;
  }
}

static void queue_redraw(AppState* s) {
  if (s && s->drawing && GTK_IS_WIDGET(s->drawing))
    gtk_widget_queue_draw(s->drawing);
}

static void on_main_window_destroy(GtkWidget*, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (s) {
    s->window = nullptr;
    s->scrolled = nullptr;
    s->drawing = nullptr;
  }
  gtk_main_quit();
}

// ===== Helper: ESC closes dialogs reliably
static gboolean dialog_esc_to_cancel(GtkWidget* widget, GdkEventKey* ev, gpointer) {
  if (ev && ev->keyval == GDK_KEY_Escape) {
    gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_CANCEL);
    return TRUE;
  }
  return FALSE;
}


static void open_setlist_row_activated(GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*, gpointer user_data) {
  GtkWidget* dlg = GTK_WIDGET(user_data);
  if (dlg) gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
}

static gboolean open_setlist_key_press(GtkWidget*, GdkEventKey* ev, gpointer user_data) {
  GtkWidget* dlg = GTK_WIDGET(user_data);
  if (!dlg || !ev) return FALSE;
  if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
    gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    return TRUE;
  }
  return FALSE;
}


// ===== Zoom overlay (B)
static gboolean zoom_overlay_timeout_cb(gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (!s) return G_SOURCE_REMOVE;
  s->zoom_overlay = false;
  s->zoom_overlay_timer = 0;
  queue_redraw(s);
  return G_SOURCE_REMOVE;
}

static void trigger_zoom_overlay(AppState* s) {
  if (!s) return;
  s->zoom_overlay = true;
  s->zoom_overlay_percent = s->zoom_percent;

  if (s->zoom_overlay_timer) {
    g_source_remove(s->zoom_overlay_timer);
    s->zoom_overlay_timer = 0;
  }
  s->zoom_overlay_timer = g_timeout_add(900, zoom_overlay_timeout_cb, s);
  queue_redraw(s);
}


static gboolean page_overlay_timeout_cb(gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (!s) return G_SOURCE_REMOVE;
  s->page_overlay = false;
  s->page_overlay_timer = 0;
  queue_redraw(s);
  return G_SOURCE_REMOVE;
}

static void trigger_page_overlay(AppState* s) {
  if (!s) return;

  int left = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
  int right = (s->two_pages && left + 1 < s->n_pages) ? (left + 1) : -1;

  // Human-friendly 1-based page numbers for display
  if (right >= 0) {
    s->page_overlay_text = "Pages " + std::to_string(left + 1) + "-" + std::to_string(right + 1) +
                           " / " + std::to_string(s->n_pages);
  } else {
    s->page_overlay_text = "Page " + std::to_string(left + 1) +
                           " / " + std::to_string(s->n_pages);
  }

  s->page_overlay = true;

  if (s->page_overlay_timer) {
    g_source_remove(s->page_overlay_timer);
    s->page_overlay_timer = 0;
  }
  s->page_overlay_timer = g_timeout_add(900, page_overlay_timeout_cb, s);
  queue_redraw(s);
}

// viewport size (visible area of scrolled window)
static void get_viewport_size(AppState* s, int& vw, int& vh) {
  vw = 1200; vh = 800;
  if (!s || !s->scrolled) return;

  GtkAdjustment* hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  if (hadj && vadj) {
    double pw = gtk_adjustment_get_page_size(hadj);
    double ph = gtk_adjustment_get_page_size(vadj);
    if (pw >= 50) vw = (int)pw;
    if (ph >= 50) vh = (int)ph;
  }
}

static void center_view(AppState* s) {
  if (!s || !s->scrolled) return;
  GtkAdjustment* hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  if (!hadj || !vadj) return;

  // Center within the scrollable area when content is larger than the viewport.
  const double h_upper = gtk_adjustment_get_upper(hadj);
  const double h_page  = gtk_adjustment_get_page_size(hadj);
  const double v_upper = gtk_adjustment_get_upper(vadj);
  const double v_page  = gtk_adjustment_get_page_size(vadj);

  double target_x = 0.0;
  double target_y = 0.0;

  if (h_upper > h_page) target_x = (h_upper - h_page) * 0.5;
  if (v_upper > v_page) target_y = (v_upper - v_page) * 0.5;

  gtk_adjustment_set_value(hadj, target_x);
  gtk_adjustment_set_value(vadj, target_y);
}

static void compute_content_size(AppState* s) {
  if (!s || !s->doc || s->n_pages <= 0) return;

  int VW, VH;
  get_viewport_size(s, VW, VH);

  const int margin = 12;
  const int gap = 12;

  int left_idx = clampi(s->current_left, 0, s->n_pages - 1);
  int right_idx = (s->two_pages && left_idx + 1 < s->n_pages) ? left_idx + 1 : -1;

  PopplerPage* left = poppler_document_get_page(s->doc, left_idx);
  if (!left) return;

  PopplerPage* right = (right_idx >= 0) ? poppler_document_get_page(s->doc, right_idx) : nullptr;

  double lw=0, lh=0;
  poppler_page_get_size(left, &lw, &lh);

  double rw=0, rh=0;
  if (right) poppler_page_get_size(right, &rw, &rh);

  double scaleFit = 1.0;

  if (!s->two_pages || !right) {
    const double availW = std::max(1.0, (double)VW - 2.0 * margin);
    const double availH = std::max(1.0, (double)VH - 2.0 * margin);
    scaleFit = std::min(availW / lw, availH / lh);
  } else {
    const double availW = std::max(1.0, (double)VW - 2.0 * margin - gap);
    const double availH = std::max(1.0, (double)VH - 2.0 * margin);
    const double scaleH_left = availH / lh;
    const double scaleH_right = availH / rh;
    scaleFit = std::min(scaleH_left, scaleH_right);

    const double totalW_at_scale = (lw * scaleFit) + gap + (rw * scaleFit);
    if (totalW_at_scale > availW) {
      scaleFit = availW / (lw + rw);
      scaleFit = std::min(scaleFit, std::min(scaleH_left, scaleH_right));
    }
  }

  const double scale = scaleFit * s->zoom;

  double contentW = 0, contentH = 0;
  if (!s->two_pages || !right) {
    contentW = 2.0 * margin + lw * scale;
    contentH = 2.0 * margin + lh * scale;
  } else {
    contentW = 2.0 * margin + (lw * scale) + gap + (rw * scale);
    contentH = 2.0 * margin + std::max(lh * scale, rh * scale);
  }

  if (right) g_object_unref(right);
  g_object_unref(left);

  s->contentW = std::max(VW, (int)std::ceil(contentW));
  s->contentH = std::max(VH, (int)std::ceil(contentH));

  gtk_widget_set_size_request(s->drawing, s->contentW, s->contentH);
}

static void goto_left_page(AppState* s, int left0) {
  if (!s || !s->doc) return;
  s->current_left = clampi(left0, 0, std::max(0, s->n_pages - 1));
  normalize_left(s);
  compute_content_size(s);
  update_status_label(s);
  trigger_page_overlay(s);
  queue_redraw(s);
}

static void next_page(AppState* s) {
  int step = s->two_pages ? 2 : 1;
  int t = s->current_left + step;
  if (t >= s->n_pages) t = s->n_pages - 1;
  goto_left_page(s, t);
}

static void prev_page(AppState* s) {
  int step = s->two_pages ? 2 : 1;
  int t = s->current_left - step;
  if (t < 0) t = 0;
  goto_left_page(s, t);
}

static void zoom_in(AppState* s)  {
  s->zoom = clampd(s->zoom * 1.10, 0.30, 5.00);
  update_zoom_percent(s);
  trigger_zoom_overlay(s);
  compute_content_size(s);
  update_status_label(s);
  trigger_page_overlay(s);
  queue_redraw(s);
}
static void zoom_out(AppState* s) {
  s->zoom = clampd(s->zoom / 1.10, 0.30, 5.00);
  update_zoom_percent(s);
  trigger_zoom_overlay(s);
  compute_content_size(s);
  update_status_label(s);
  trigger_page_overlay(s);
  queue_redraw(s);
}
static void zoom_reset(AppState* s){
  s->zoom = 1.0;
  update_zoom_percent(s);
  trigger_zoom_overlay(s);
  compute_content_size(s);
  update_status_label(s);
  trigger_page_overlay(s);
  queue_redraw(s);
}

static void scroll_by(AppState* s, double dx, double dy) {
  if (!s || !s->scrolled) return;
  GtkAdjustment* hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  if (hadj) gtk_adjustment_set_value(hadj, clampd(gtk_adjustment_get_value(hadj) + dx,
                                                 gtk_adjustment_get_lower(hadj),
                                                 gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj)));
  if (vadj) gtk_adjustment_set_value(vadj, clampd(gtk_adjustment_get_value(vadj) + dy,
                                                 gtk_adjustment_get_lower(vadj),
                                                 gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj)));
}

// ===== Smart advance (C): when zoom>100%, Space/PageDown advances by ~1 screen until bottom, then next page.
static bool vscroll_info(AppState* s, double& v, double& lower, double& upper, double& page) {
  if (!s || !s->scrolled) return false;
  GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  if (!vadj) return false;
  v = gtk_adjustment_get_value(vadj);
  lower = gtk_adjustment_get_lower(vadj);
  upper = gtk_adjustment_get_upper(vadj);
  page = gtk_adjustment_get_page_size(vadj);
  return true;
}

static void vscroll_set(AppState* s, double nv) {
  if (!s || !s->scrolled) return;
  GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scrolled));
  if (!vadj) return;
  double lower = gtk_adjustment_get_lower(vadj);
  double maxv  = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);
  gtk_adjustment_set_value(vadj, clampd(nv, lower, maxv));
}

static void smart_advance(AppState* s) {
  if (!s) return;

  if (s->zoom <= 1.000001) { // classic
    next_page(s);
    return;
  }

  double v=0, lower=0, upper=0, page=0;
  if (!vscroll_info(s, v, lower, upper, page)) {
    next_page(s);
    return;
  }

  const double maxv = upper - page;
  const double eps = 2.0;

  if (maxv > lower + eps && v < maxv - eps) {
    vscroll_set(s, v + page * 0.90);
    return;
  }

  next_page(s);
  vscroll_set(s, lower);
}

// ===== Dialog helpers
static bool ask_int(AppState* s, const char* title, const char* label_txt, int& outVal) {
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      title,
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Annuler", GTK_RESPONSE_CANCEL,
      "_OK", GTK_RESPONSE_OK,
      nullptr);

  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  gtk_container_add(GTK_CONTAINER(content), box);

  GtkWidget* label = gtk_label_new(label_txt);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_NUMBER);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  dialog_begin(s, dialog);
  int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  dialog_end(s);
  bool ok = false;
  if (resp == GTK_RESPONSE_OK) {
    const char* txt = gtk_entry_get_text(GTK_ENTRY(entry));
    int v = txt ? atoi(txt) : 0;
    if (v > 0) { outVal = v; ok = true; }
  }
  gtk_widget_destroy(dialog);
  return ok;
}

static void info_box(AppState* s, const std::string& msg) {
  GtkWidget* d = gtk_message_dialog_new(
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_INFO,
      GTK_BUTTONS_CLOSE,
      "%s", msg.c_str());

  g_signal_connect(d, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  dialog_begin(s, d);


  gtk_dialog_run(GTK_DIALOG(d));


  dialog_end(s);
  gtk_widget_destroy(d);
}

// ===== Extract (E)
static std::string default_extract_name(const std::string& in_abs, int p1, int p2) {
  char* base = g_path_get_basename(in_abs.c_str()); // ex: Meditation.pdf
  std::string b(base ? base : "score.pdf");
  if (base) g_free(base);

  if (b.size() >= 4) {
    std::string tail = b.substr(b.size() - 4);
    for (auto& c : tail) c = (char)tolower(c);
    if (tail == ".pdf") b = b.substr(0, b.size() - 4);
  }
  return b + "_p" + std::to_string(p1) + "-p" + std::to_string(p2) + ".pdf";
}

static bool choose_save_path(AppState* s, const std::string& default_dir, const std::string& default_name, std::string& out_path) {
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      "Enregistrer l'extraction",
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      "_Annuler", GTK_RESPONSE_CANCEL,
      "_Enregistrer", GTK_RESPONSE_ACCEPT,
      nullptr);

  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkFileChooser* ch = GTK_FILE_CHOOSER(dlg);
  gtk_file_chooser_set_do_overwrite_confirmation(ch, TRUE);
  gtk_file_chooser_set_current_folder(ch, default_dir.c_str());
  gtk_file_chooser_set_current_name(ch, default_name.c_str());

  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "PDF (*.pdf)");
  gtk_file_filter_add_pattern(f, "*.pdf");
  gtk_file_chooser_add_filter(ch, f);

  bool ok = false;
  dialog_begin(s, dlg);

  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);

  if (resp == GTK_RESPONSE_ACCEPT) {
    char* fn = gtk_file_chooser_get_filename(ch);
    if (fn) {
      out_path = fn;
      g_free(fn);
      ok = true;
    }
  }
  gtk_widget_destroy(dlg);
  return ok;
}

static bool run_qpdf_extract(AppState* s, const std::string& in_abs, int page_from_1, int page_to_1, const std::string& out_abs) {
  if (out_abs == in_abs) {
    info_box(s, "Refus: impossible d'écraser le fichier source.");
    return false;
  }

  std::string range = std::to_string(page_from_1) + "-" + std::to_string(page_to_1);

  // qpdf "in.pdf" --pages "in.pdf" 24-30 -- "out.pdf"
  std::string cmd =
      "qpdf \"" + in_abs + "\" --pages \"" + in_abs + "\" " + range + " -- \"" + out_abs + "\"";

  int rc = system(cmd.c_str());
  if (rc != 0) {
    info_box(s, "Extraction échouée.\n"
                "Vérifie que qpdf est installé : sudo apt install qpdf");
    return false;
  }

  info_box(s, "PDF extrait enregistré:\n" + out_abs);
  return true;
}

static void extract_pages(AppState* s) {
  if (!s || !s->window || s->n_pages <= 0) return;

  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Extraction PDF",
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_OK", GTK_RESPONSE_OK,
      nullptr);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  gtk_container_add(GTK_CONTAINER(content), box);

  GtkWidget* label = gtk_label_new("Page or range (examples: 7 or 10-14):");
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  dialog_begin(s, dialog);
  int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  dialog_end(s);

  int p1 = 0, p2 = 0;
  bool ok = false;
  if (resp == GTK_RESPONSE_OK) {
    const char* txt = gtk_entry_get_text(GTK_ENTRY(entry));
    std::string spec = txt ? txt : "";
    spec.erase(std::remove_if(spec.begin(), spec.end(), [](unsigned char c){ return std::isspace(c); }), spec.end());
    if (!spec.empty()) {
      size_t dash = spec.find('-');
      try {
        if (dash == std::string::npos) {
          p1 = p2 = std::stoi(spec);
          ok = true;
        } else {
          p1 = std::stoi(spec.substr(0, dash));
          p2 = std::stoi(spec.substr(dash + 1));
          ok = true;
        }
      } catch (...) {
        ok = false;
      }
    }
  }
  gtk_widget_destroy(dialog);
  if (!ok) return;

  if (p1 < 1 || p2 < 1 || p1 > s->n_pages || p2 > s->n_pages) {
    info_box(s, std::string("Pages hors limites.\nRappel: 1 <= début <= fin <= ") + std::to_string(s->n_pages));
    return;
  }
  if (p2 < p1) {
    info_box(s, "Intervalle invalide: il faut début <= fin.");
    return;
  }

  char* dir = g_path_get_dirname(s->input_pdf_abs.c_str());
  std::string default_dir = dir ? dir : ".";
  if (dir) g_free(dir);

  std::string default_name = default_extract_name(s->input_pdf_abs, p1, p2);

  std::string out_path;
  if (!choose_save_path(s, default_dir, default_name, out_path)) return;

  if (out_path.size() < 4 || out_path.substr(out_path.size() - 4) != ".pdf") {
    out_path += ".pdf";
  }

  run_qpdf_extract(s, s->input_pdf_abs, p1, p2, out_path);
}

static void show_help(AppState* s) {
  if (!s || !s->window) return;

  const int left1  = s->current_left + 1;
  const int right1 = (s->two_pages && (s->current_left + 1) < s->n_pages) ? (s->current_left + 2) : -1;

  std::string pageLine;
  if (right1 > 0) {
    pageLine = "Page(s): " + std::to_string(left1) + "-" + std::to_string(right1) +
               " / " + std::to_string(s->n_pages);
  } else {
    pageLine = "Page: " + std::to_string(left1) + " / " + std::to_string(s->n_pages);
  }

  std::string help =
      std::string("rdScore ") + RDSCORE_VERSION + "\n\n"
      + pageLine + "\n"
      "Zoom courant : " + std::to_string(s->zoom_percent) + " %\n\n"
      "Pages:\n"
      "  ←/→ : page (si zoom=100%)\n"
      "  PageUp/PageDown : page (toujours)\n"
      "  Space/Backspace : page (toujours)\n\n"
      "Zoom:\n"
      "  Ctrl + / Ctrl - : zoom +/-\n"
      "  Ctrl 0          : zoom 100%\n\n"
      "Scroll (si zoom > 100%):\n"
      "  ←/→ : scroll horizontal\n"
      "  ↑/↓ : scroll vertical\n\n"
      "Mode:\n"
      "  1 / 2 : 1 page / 2 pages\n"
      "  f     : plein écran\n"
      "  g     : aller à la page\n"
      "  e     : extraire pages -> nouveau PDF\n"
      "  Ctrl+P: imprimer\n"
      "  Esc   : fermer le PDF / quitter";

  GtkWidget* d = gtk_message_dialog_new(
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_INFO,
      GTK_BUTTONS_CLOSE,
      "%s",
      help.c_str());

  g_signal_connect(d, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  dialog_begin(s, d);


  gtk_dialog_run(GTK_DIALOG(d));


  dialog_end(s);
  gtk_widget_destroy(d);
}

static bool choose_open_pdf(AppState* s);
static void print_document(AppState* s);
static bool open_setlist_dialog(AppState* s);
static void close_current_document(AppState* s);
static void create_setlist_dialog(AppState* s);
static void edit_setlist_dialog(AppState* s);
static void rename_setlist_dialog(AppState* s);
static void delete_setlist_dialog(AppState* s);

static void goto_dialog(AppState* s) {
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Aller à la page",
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Annuler", GTK_RESPONSE_CANCEL,
      "_Aller", GTK_RESPONSE_OK,
      nullptr);

  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  gtk_container_add(GTK_CONTAINER(content), box);

  GtkWidget* label = gtk_label_new("Numéro de page (1-based) :");
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_NUMBER);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  dialog_begin(s, dialog);
  int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  dialog_end(s);
  if (resp == GTK_RESPONSE_OK) {
    const char* txt = gtk_entry_get_text(GTK_ENTRY(entry));
    int page = txt ? atoi(txt) : 0;
    if (page >= 1 && page <= s->n_pages) goto_left_page(s, page - 1);
  }
  gtk_widget_destroy(dialog);
}

static void open_top_menu(AppState* s, GtkWidget* item) {
  if (!s || !s->menubar || !item) return;
  gtk_widget_grab_focus(s->menubar);
  gtk_menu_shell_select_item(GTK_MENU_SHELL(s->menubar), item);
  gtk_menu_item_activate(GTK_MENU_ITEM(item));
}

static gboolean on_key(GtkWidget*, GdkEventKey* ev, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (!s) return FALSE;

  const bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
  const bool alt  = (ev->state & GDK_MOD1_MASK) != 0;

  if (alt && !ctrl) {
    switch (ev->keyval) {
      case GDK_KEY_f:
      case GDK_KEY_F:
        open_top_menu(s, s->menu_file); return TRUE;
      case GDK_KEY_s:
      case GDK_KEY_S:
        open_top_menu(s, s->menu_setlists); return TRUE;
      case GDK_KEY_h:
      case GDK_KEY_H:
        open_top_menu(s, s->menu_help); return TRUE;
      default:
        return FALSE;
    }
  }

  // Shortcuts with Ctrl
  if (ctrl) {
    switch (ev->keyval) {
      case GDK_KEY_o:
      case GDK_KEY_O:
        choose_open_pdf(s); return TRUE;
      case GDK_KEY_p:
      case GDK_KEY_P:
        print_document(s); return TRUE;
      case GDK_KEY_plus:
      case GDK_KEY_KP_Add:
      case GDK_KEY_equal: // some layouts
        zoom_in(s); return TRUE;
      case GDK_KEY_minus:
      case GDK_KEY_KP_Subtract:
        zoom_out(s); return TRUE;
      case GDK_KEY_0:
      case GDK_KEY_KP_0:
        zoom_reset(s); return TRUE;
      default: break;
    }
  }

  // When zoom > 1 : arrows scroll (page keys remain page nav)
  if (s->zoom > 1.000001) {
    const double step = 90.0;
    switch (ev->keyval) {
      case GDK_KEY_Left:  scroll_by(s, -step, 0); return TRUE;
      case GDK_KEY_Right: scroll_by(s, +step, 0); return TRUE;
      case GDK_KEY_Up:    scroll_by(s, 0, -step); return TRUE;
      case GDK_KEY_Down:  scroll_by(s, 0, +step); return TRUE;
      default: break;
    }
  }

  switch (ev->keyval) {
    case GDK_KEY_Escape:
      if (s->doc) {
        close_current_document(s);
      } else {
        gtk_main_quit();
      }
      return TRUE;

    case GDK_KEY_f:
    case GDK_KEY_F:
      toggle_fullscreen(s);
      return TRUE;

    case GDK_KEY_question:
    case GDK_KEY_F1:
      show_help(s);
      return TRUE;

    case GDK_KEY_g:
    case GDK_KEY_G:
      goto_dialog(s);
      return TRUE;

    case GDK_KEY_e:
    case GDK_KEY_E:
      extract_pages(s);
      return TRUE;

    case GDK_KEY_1:
    case GDK_KEY_KP_1:
      s->two_pages = false;
      normalize_left(s);
      compute_content_size(s);
      trigger_page_overlay(s);
      queue_redraw(s);
      return TRUE;

    case GDK_KEY_2:
    case GDK_KEY_KP_2:
      s->two_pages = true;
      normalize_left(s);
      compute_content_size(s);
      trigger_page_overlay(s);
      queue_redraw(s);
      return TRUE;

    // Page navigation (always) — smart advance on forward keys
    case GDK_KEY_Page_Down:
    case GDK_KEY_space:
    case GDK_KEY_Right:   // only reaches here when zoom==1
      smart_advance(s);
      return TRUE;

    case GDK_KEY_Page_Up:
    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:    // only reaches here when zoom==1
      prev_page(s);
      return TRUE;

    default:
      return FALSE;
  }
}

static void render_page(PopplerPage* page, cairo_t* cr, double x, double y, double scale) {
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, scale, scale);
  poppler_page_render(page, cr);
  cairo_restore(cr);
}

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (!s) return FALSE;

  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  const int W = std::max(1, a.width);
  const int H = std::max(1, a.height);

  cairo_save(cr);
  cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
  cairo_paint(cr);
  cairo_restore(cr);

  if (!s->doc || s->n_pages <= 0) return FALSE;

  int VW, VH;
  get_viewport_size(s, VW, VH);

  const int margin = 12;
  const int gap = 12;

  int left_idx = clampi(s->current_left, 0, s->n_pages - 1);
  int right_idx = (s->two_pages && left_idx + 1 < s->n_pages) ? left_idx + 1 : -1;

  PopplerPage* left = poppler_document_get_page(s->doc, left_idx);
  if (!left) return FALSE;
  PopplerPage* right = (right_idx >= 0) ? poppler_document_get_page(s->doc, right_idx) : nullptr;

  double lw=0, lh=0;
  poppler_page_get_size(left, &lw, &lh);

  double rw=0, rh=0;
  if (right) poppler_page_get_size(right, &rw, &rh);

  double scaleFit = 1.0;
  if (!s->two_pages || !right) {
    const double availW = std::max(1.0, (double)VW - 2.0 * margin);
    const double availH = std::max(1.0, (double)VH - 2.0 * margin);
    scaleFit = std::min(availW / lw, availH / lh);
  } else {
    const double availW = std::max(1.0, (double)VW - 2.0 * margin - gap);
    const double availH = std::max(1.0, (double)VH - 2.0 * margin);
    const double scaleH_left = availH / lh;
    const double scaleH_right = availH / rh;
    scaleFit = std::min(scaleH_left, scaleH_right);

    const double totalW_at_scale = (lw * scaleFit) + gap + (rw * scaleFit);
    if (totalW_at_scale > availW) {
      scaleFit = availW / (lw + rw);
      scaleFit = std::min(scaleFit, std::min(scaleH_left, scaleH_right));
    }
  }

  const double scale = scaleFit * s->zoom;

  if (!s->two_pages || !right) {
    const double drawW = lw * scale;
    const double drawH = lh * scale;

    const double contentW = 2.0 * margin + drawW;
    const double contentH = 2.0 * margin + drawH;

    const double x0 = (W > contentW) ? ((W - contentW) / 2.0) : 0.0;
    const double y0 = (H > contentH) ? ((H - contentH) / 2.0) : 0.0;

    const double x = x0 + margin;
    const double y = y0 + margin;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, x, y, drawW, drawH);
    cairo_fill(cr);
    cairo_restore(cr);

    render_page(left, cr, x, y, scale);
  } else {
    const double drawLW = lw * scale;
    const double drawLH = lh * scale;
    const double drawRW = rw * scale;
    const double drawRH = rh * scale;

    const double totalDrawW = drawLW + gap + drawRW;
    const double maxDrawH = std::max(drawLH, drawRH);

    const double contentW = 2.0 * margin + totalDrawW;
    const double contentH = 2.0 * margin + maxDrawH;

    const double x0 = (W > contentW) ? ((W - contentW) / 2.0) : 0.0;
    const double y0 = (H > contentH) ? ((H - contentH) / 2.0) : 0.0;

    const double startX = x0 + margin;
    const double yL = y0 + margin + (maxDrawH - drawLH) / 2.0;
    const double yR = y0 + margin + (maxDrawH - drawRH) / 2.0;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, startX, yL, drawLW, drawLH);
    cairo_fill(cr);
    cairo_rectangle(cr, startX + drawLW + gap, yR, drawRW, drawRH);
    cairo_fill(cr);
    cairo_restore(cr);

    render_page(left, cr, startX, yL, scale);
    render_page(right, cr, startX + drawLW + gap, yR, scale);
  }

  if (right) g_object_unref(right);
  g_object_unref(left);

  // ===== Draw zoom overlay (B)
  if (s->zoom_overlay) {
    /* visual zoom overlay disabled in test v5h */
  }


  if (s->page_overlay) {
    /* visual page overlay disabled in test v5h */
  }


  return FALSE;
}

// Apply fullscreen only after realize
static void on_realize(GtkWidget* widget, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (s && s->fullscreen) {
    gtk_window_fullscreen(GTK_WINDOW(widget));
    show_cursor(widget);
  }
  if (s) compute_content_size(s);
}

static void on_size_allocate(GtkWidget*, GdkRectangle*, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (!s) return;
  compute_content_size(s);
  if (s->pending_recenter) {
    center_view(s);
    s->pending_recenter = false;
    queue_redraw(s);
  }
}



static void unload_document(AppState* s) {
  if (!s) return;
  if (s->doc) {
    g_object_unref(s->doc);
    s->doc = nullptr;
  }
  s->n_pages = 0;
  s->current_left = 0;
  s->input_pdf_abs.clear();
  s->contentW = 1200;
  s->contentH = 800;
  if (s->drawing && GTK_IS_WIDGET(s->drawing))
    gtk_widget_set_size_request(s->drawing, s->contentW, s->contentH);
  update_status_label(s);
  queue_redraw(s);
}

static bool reopen_active_setlist(AppState* s);

static void close_current_document(AppState* s) {
  if (!s) return;
  const bool from_setlist = s->current_doc_from_setlist && !s->active_setlist_path.empty();
  unload_document(s);
  show_cursor(s->window);
  restore_focus(s);
  if (from_setlist) {
    s->current_doc_from_setlist = false;
    reopen_active_setlist(s);
  } else {
    s->current_doc_from_setlist = false;
  }
}

static bool load_document_from_path(AppState* s, const std::string& path, bool show_errors = true, bool from_setlist = false) {
  if (!s) return false;
  char* abs_path = g_canonicalize_filename(path.c_str(), nullptr);
  if (!abs_path) {
    if (show_errors) info_box(s, "Invalid path.");
    return false;
  }

  char* uri = g_filename_to_uri(abs_path, nullptr, nullptr);
  if (!uri) {
    if (show_errors) info_box(s, "Bad path.");
    g_free(abs_path);
    return false;
  }

  GError* err = nullptr;
  PopplerDocument* new_doc = poppler_document_new_from_file(uri, nullptr, &err);
  g_free(uri);

  if (!new_doc) {
    if (show_errors) {
      std::string msg = "Impossible d'ouvrir le PDF: ";
      msg += (err ? err->message : "unknown error");
      info_box(s, msg);
    }
    if (err) g_error_free(err);
    g_free(abs_path);
    return false;
  }

  const int n_pages = poppler_document_get_n_pages(new_doc);
  if (n_pages <= 0) {
    if (show_errors) info_box(s, "PDF sans pages.");
    g_object_unref(new_doc);
    g_free(abs_path);
    return false;
  }

  unload_document(s);
  s->doc = new_doc;
  s->n_pages = n_pages;
  s->input_pdf_abs = abs_path;
  g_free(abs_path);
  s->current_left = 0;
  s->current_doc_from_setlist = from_setlist;
  normalize_left(s);
  compute_content_size(s);
  update_status_label(s);
  trigger_page_overlay(s);
  queue_redraw(s);
  return true;
}

static bool choose_open_pdf(AppState* s) {
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      "Open PDF",
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open", GTK_RESPONSE_ACCEPT,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);
  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "PDF (*.pdf)");
  gtk_file_filter_add_pattern(f, "*.pdf");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);
  bool ok = false;
  if (resp == GTK_RESPONSE_ACCEPT) {
    char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    if (fn) {
      char* dir = g_path_get_dirname(fn);
      if (dir) {
        s->last_pdf_dir = dir;
        g_free(dir);
      }
      s->active_setlist_path.clear();
      s->current_doc_from_setlist = false;
      ok = load_document_from_path(s, fn, true, false);
      g_free(fn);
    }
  }
  gtk_widget_destroy(dlg);
  return ok;
}

static std::vector<std::string> parse_setlist_file(const std::string& path) {
  std::vector<std::string> items;
  std::ifstream in(path);
  if (!in) return items;
  std::string line;
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (line.empty()) continue;
    if (line[0] == '#') continue;
    if (line[0] != '/') continue; // absolute paths only
    items.push_back(line);
  }
  return items;
}


static bool choose_setlist_file(AppState* s, const char* title, std::string& out_path) {
  GtkWidget* choose = gtk_file_chooser_dialog_new(
      title,
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open", GTK_RESPONSE_ACCEPT,
      nullptr);

  std::string dir = get_setlists_directory();
  if (!dir.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(choose), dir.c_str());

  g_signal_connect(choose, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);
  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "Setlists (*.txt, *.lst, *.setlist)");
  gtk_file_filter_add_pattern(f, "*.txt");
  gtk_file_filter_add_pattern(f, "*.lst");
  gtk_file_filter_add_pattern(f, "*.setlist");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(choose), f);

  bool ok = false;
  dialog_begin(s, choose);
  int resp = gtk_dialog_run(GTK_DIALOG(choose));
  dialog_end(s);

  if (resp == GTK_RESPONSE_ACCEPT) {
    char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(choose));
    if (fn) {
      out_path = fn;
      g_free(fn);
      ok = true;
    }
  }
  gtk_widget_destroy(choose);
  return ok;
}

static bool choose_new_setlist_path(AppState* s, std::string& out_path) {
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      "Create setlist",
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Create", GTK_RESPONSE_ACCEPT,
      nullptr);

  std::string dir = get_setlists_directory();
  if (!dir.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir.c_str());
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "new_setlist.lst");

  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);
  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "Setlists (*.lst)");
  gtk_file_filter_add_pattern(f, "*.lst");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);

  bool ok = false;
  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);

  if (resp == GTK_RESPONSE_ACCEPT) {
    char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    if (fn) {
      out_path = fn;
      g_free(fn);
      if (out_path.size() < 4 || out_path.substr(out_path.size() - 4) != ".lst")
        out_path += ".lst";
      ok = true;
    }
  }
  gtk_widget_destroy(dlg);
  return ok;
}

static bool choose_pdf_path(AppState* s, std::string& out_path) {
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      "Choose PDF",
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open", GTK_RESPONSE_ACCEPT,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "PDF (*.pdf)");
  gtk_file_filter_add_pattern(f, "*.pdf");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);

  if (!s->last_pdf_dir.empty()) {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), s->last_pdf_dir.c_str());
  } else if (!s->input_pdf_abs.empty()) {
    char* dir = g_path_get_dirname(s->input_pdf_abs.c_str());
    if (dir) {
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
      g_free(dir);
    }
  }

  bool ok = false;
  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);

  if (resp == GTK_RESPONSE_ACCEPT) {
    char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    if (fn) {
      out_path = fn;
      char* dir = g_path_get_dirname(fn);
      if (dir) {
        s->last_pdf_dir = dir;
        g_free(dir);
      }
      g_free(fn);
      ok = !out_path.empty() && out_path[0] == '/';
    }
  }
  gtk_widget_destroy(dlg);
  return ok;
}

static bool choose_pdf_paths(AppState* s, std::vector<std::string>& out_paths) {
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      "Choose PDF(s)",
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open", GTK_RESPONSE_ACCEPT,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "PDF (*.pdf)");
  gtk_file_filter_add_pattern(f, "*.pdf");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);

  if (!s->last_pdf_dir.empty()) {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), s->last_pdf_dir.c_str());
  } else if (!s->input_pdf_abs.empty()) {
    char* dir = g_path_get_dirname(s->input_pdf_abs.c_str());
    if (dir) {
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
      g_free(dir);
    }
  }

  bool ok = false;
  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);

  if (resp == GTK_RESPONSE_ACCEPT) {
    GSList* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
    for (GSList* p = files; p; p = p->next) {
      char* fn = (char*)p->data;
      if (fn) {
        std::string path = fn;
        if (!path.empty() && path[0] == '/') out_paths.push_back(path);
        g_free(fn);
      }
    }
    g_slist_free(files);

    if (!out_paths.empty()) {
      char* dir = g_path_get_dirname(out_paths.front().c_str());
      if (dir) {
        s->last_pdf_dir = dir;
        g_free(dir);
      }
      ok = true;
    }
  }

  gtk_widget_destroy(dlg);
  return ok;
}

static std::vector<std::string> collect_store_strings(GtkListStore* store) {
  std::vector<std::string> out;
  GtkTreeModel* model = GTK_TREE_MODEL(store);
  GtkTreeIter it;
  gboolean valid = gtk_tree_model_get_iter_first(model, &it);
  while (valid) {
    gchar* val = nullptr;
    gtk_tree_model_get(model, &it, 0, &val, -1);
    if (val) {
      out.push_back(val);
      g_free(val);
    }
    valid = gtk_tree_model_iter_next(model, &it);
  }
  return out;
}

static void save_setlist_file(const std::string& path, const std::vector<std::string>& items) {
  std::ofstream out(path);
  for (const auto& s : items) out << s << "\n";
}

static int get_selected_row_index(GtkTreeView* view) {
  GtkTreeSelection* sel = gtk_tree_view_get_selection(view);
  GtkTreeModel* model = nullptr;
  GtkTreeIter it;
  if (!gtk_tree_selection_get_selected(sel, &model, &it)) return -1;
  GtkTreePath* path = gtk_tree_model_get_path(model, &it);
  if (!path) return -1;
  int* indices = gtk_tree_path_get_indices(path);
  int idx = (indices ? indices[0] : -1);
  gtk_tree_path_free(path);
  return idx;
}

static void set_cursor_to_row(GtkTreeView* view, int idx) {
  if (idx < 0) return;
  GtkTreePath* path = gtk_tree_path_new_from_indices(idx, -1);
  gtk_tree_view_set_cursor(view, path, nullptr, FALSE);
  gtk_tree_view_scroll_to_cell(view, path, nullptr, TRUE, 0.5f, 0.0f);
  gtk_tree_path_free(path);
}

static bool edit_setlist_file(AppState* s, const std::string& setlist_path, std::vector<std::string> items, bool creating) {
  enum {
    RESP_ADD = 1001,
    RESP_DELETE = 1002,
    RESP_UP = 1003,
    RESP_DOWN = 1004
  };

  GtkWidget* dlg = gtk_dialog_new_with_buttons(
      creating ? "Create setlist" : "Edit setlist",
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "Add before", RESP_ADD,
      "Delete", RESP_DELETE,
      "Move up", RESP_UP,
      "Move down", RESP_DOWN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      creating ? "_Create" : "_Save", GTK_RESPONSE_OK,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  gtk_container_add(GTK_CONTAINER(content), box);

  std::string label_text = basename_only(setlist_path);
  GtkWidget* label = gtk_label_new(label_text.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkListStore* store = gtk_list_store_new(1, G_TYPE_STRING);
  for (const auto& p : items) {
    GtkTreeIter it;
    gtk_list_store_append(store, &it);
    gtk_list_store_set(store, &it, 0, p.c_str(), -1);
  }

  GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  GtkCellRenderer* r = gtk_cell_renderer_text_new();
  GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes("PDF", r, "text", 0, nullptr);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), TRUE);
  GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_BROWSE);

  GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_widget_set_size_request(sw, 760, 320);
  gtk_container_add(GTK_CONTAINER(sw), view);
  gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

  gtk_widget_show_all(dlg);
  if (!items.empty()) set_cursor_to_row(GTK_TREE_VIEW(view), 0);

  bool saved = false;

  while (true) {
    dialog_begin(s, dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    dialog_end(s);

    if (resp == GTK_RESPONSE_OK) {
      auto out_items = collect_store_strings(store);
      save_setlist_file(setlist_path, out_items);
      saved = true;
      break;
    }
    if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
      break;
    }

    if (resp == RESP_ADD) {
      std::vector<std::string> pdfs;
      if (!choose_pdf_paths(s, pdfs)) continue;

      int idx = get_selected_row_index(GTK_TREE_VIEW(view));
      int count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), nullptr);
      if (idx < 0 || idx > count) idx = count;

      int insert_at = idx;
      for (const auto& pdf : pdfs) {
        GtkTreeIter it;
        int current_count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), nullptr);
        if (insert_at >= current_count) gtk_list_store_append(store, &it);
        else gtk_list_store_insert(store, &it, insert_at);
        gtk_list_store_set(store, &it, 0, pdf.c_str(), -1);
        ++insert_at;
      }

      set_cursor_to_row(GTK_TREE_VIEW(view), insert_at - 1);
      continue;
    }

    if (resp == RESP_DELETE) {
      GtkTreeModel* model = nullptr;
      GtkTreeIter it;
      if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        int idx = get_selected_row_index(GTK_TREE_VIEW(view));
        gtk_list_store_remove(store, &it);
        int count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), nullptr);
        if (count > 0) {
          if (idx >= count) idx = count - 1;
          set_cursor_to_row(GTK_TREE_VIEW(view), idx);
        }
      }
      continue;
    }

    if (resp == RESP_UP || resp == RESP_DOWN) {
      auto vec = collect_store_strings(store);
      int idx = get_selected_row_index(GTK_TREE_VIEW(view));
      if (idx < 0 || idx >= (int)vec.size()) continue;
      int new_idx = idx + (resp == RESP_UP ? -1 : 1);
      if (new_idx < 0 || new_idx >= (int)vec.size()) continue;
      std::swap(vec[idx], vec[new_idx]);
      gtk_list_store_clear(store);
      for (const auto& p : vec) {
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, 0, p.c_str(), -1);
      }
      set_cursor_to_row(GTK_TREE_VIEW(view), new_idx);
      continue;
    }
  }

  gtk_widget_destroy(dlg);
  g_object_unref(store);
  return saved;
}

static bool confirm_delete_setlist(AppState* s, const std::string& setlist_path) {
  auto items = parse_setlist_file(setlist_path);

  GtkWidget* dlg = gtk_dialog_new_with_buttons(
      "Delete setlist",
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Delete", GTK_RESPONSE_OK,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  gtk_container_add(GTK_CONTAINER(content), box);

  std::string title = "Delete setlist:\n" + basename_only(setlist_path) + "\n\nContents:";
  GtkWidget* label = gtk_label_new(title.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_widget_set_size_request(sw, 760, 260);
  gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

  GtkWidget* tv = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
  gtk_container_add(GTK_CONTAINER(sw), tv);

  GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  std::string preview;
  if (items.empty()) preview = "(empty setlist)\n";
  else {
    for (const auto& p : items) {
      preview += p;
      preview += "\n";
    }
  }
  gtk_text_buffer_set_text(buf, preview.c_str(), -1);

  gtk_widget_show_all(dlg);
  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);
  gtk_widget_destroy(dlg);

  return resp == GTK_RESPONSE_OK;
}


static bool choose_rename_setlist_path(AppState* s, const std::string& old_path, std::string& out_path) {
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      "Rename setlist",
      GTK_WINDOW(s->window),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Rename", GTK_RESPONSE_ACCEPT,
      nullptr);

  std::string dir = get_setlists_directory();
  if (!dir.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir.c_str());
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), basename_only(old_path).c_str());

  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkFileFilter* f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "Setlists (*.txt, *.lst, *.setlist)");
  gtk_file_filter_add_pattern(f, "*.txt");
  gtk_file_filter_add_pattern(f, "*.lst");
  gtk_file_filter_add_pattern(f, "*.setlist");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);

  bool ok = false;
  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);

  if (resp == GTK_RESPONSE_ACCEPT) {
    char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    if (fn) {
      out_path = fn;
      g_free(fn);

      std::string lower = out_path;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c){ return (char)std::tolower(c); });
      bool has_ext = (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".lst") ||
                     (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".txt") ||
                     (lower.size() >= 8 && lower.substr(lower.size() - 8) == ".setlist");
      if (!has_ext) out_path += ".lst";

      ok = true;
    }
  }

  gtk_widget_destroy(dlg);
  return ok;
}

static void rename_setlist_dialog(AppState* s) {
  std::string old_path;
  if (!choose_setlist_file(s, "Rename setlist", old_path)) return;

  std::string new_path;
  if (!choose_rename_setlist_path(s, old_path, new_path)) return;

  if (new_path == old_path) return;

  if (::rename(old_path.c_str(), new_path.c_str()) == 0) {
    if (s->active_setlist_path == old_path) s->active_setlist_path = new_path;
    info_box(s, "Setlist renamed:\n" + basename_only(old_path) + "\n→ " + basename_only(new_path));
  } else {
    info_box(s, "Rename failed:\n" + old_path + "\n→ " + new_path);
  }
}

static void create_setlist_dialog(AppState* s) {
  std::string path;
  if (!choose_new_setlist_path(s, path)) return;
  std::vector<std::string> items;
  edit_setlist_file(s, path, items, true);
}

static void edit_setlist_dialog(AppState* s) {
  std::string path;
  if (!choose_setlist_file(s, "Edit setlist", path)) return;
  auto items = parse_setlist_file(path);
  edit_setlist_file(s, path, items, false);
}

static void delete_setlist_dialog(AppState* s) {
  std::string path;
  if (!choose_setlist_file(s, "Delete setlist", path)) return;
  if (!confirm_delete_setlist(s, path)) return;

  if (std::remove(path.c_str()) == 0) info_box(s, "Setlist deleted:\n" + path);
  else info_box(s, "Delete failed:\n" + path);
}


static bool open_setlist_dialog_from_path(AppState* s, const std::string& setlist_path) {
  auto items = parse_setlist_file(setlist_path);
  if (items.empty()) {
    info_box(s, "Setlist vide ou illisible.");
    return false;
  }

  GtkWidget* dlg = gtk_dialog_new_with_buttons(
      "Setlist",
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open", GTK_RESPONSE_OK,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 10);
  gtk_container_add(GTK_CONTAINER(content), box);

  GtkWidget* label = gtk_label_new("Select a PDF from the setlist:");
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkListStore* store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
  for (size_t i = 0; i < items.size(); ++i) {
    GtkTreeIter it;
    gtk_list_store_append(store, &it);
    gtk_list_store_set(store, &it, 0, (int)i + 1, 1, items[i].c_str(), -1);
  }

  GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  g_object_unref(store);
  GtkCellRenderer* r = gtk_cell_renderer_text_new();
  GtkTreeViewColumn* c1 = gtk_tree_view_column_new_with_attributes("#", r, "text", 0, nullptr);
  GtkTreeViewColumn* c2 = gtk_tree_view_column_new_with_attributes("File", r, "text", 1, nullptr);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), c1);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), c2);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), TRUE);
  GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_BROWSE);
  g_signal_connect(view, "row-activated", G_CALLBACK(open_setlist_row_activated), dlg);
  g_signal_connect(view, "key-press-event", G_CALLBACK(open_setlist_key_press), dlg);

  GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_widget_set_size_request(sw, 700, 300);
  gtk_container_add(GTK_CONTAINER(sw), view);
  gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
  gtk_widget_show_all(dlg);
  GtkTreePath* p0 = gtk_tree_path_new_first();
  gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), p0, nullptr, FALSE);
  gtk_tree_path_free(p0);
  gtk_widget_grab_focus(view);

  dialog_begin(s, dlg);
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);

  bool ok = false;
  if (resp == GTK_RESPONSE_OK) {
    GtkTreeModel* model = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
      gchar* value = nullptr;
      gtk_tree_model_get(model, &it, 1, &value, -1);
      if (value) {
        std::string chosen = value;
        g_free(value);
        if (chosen.empty() || chosen[0] != '/') {
          info_box(s, "Setlist error:\nOnly absolute paths are allowed.");
          ok = false;
        } else {
          s->active_setlist_path = setlist_path;
          ok = load_document_from_path(s, chosen, true, true);
        }
      }
    }
  }
  gtk_widget_destroy(dlg);
  return ok;
}

static bool reopen_active_setlist(AppState* s) {
  if (!s || s->active_setlist_path.empty()) return false;
  return open_setlist_dialog_from_path(s, s->active_setlist_path);
}

static bool open_setlist_dialog(AppState* s) {
  std::string setlist_path;
  if (!choose_setlist_file(s, "Open setlist", setlist_path)) return false;
  return open_setlist_dialog_from_path(s, setlist_path);
}

struct PrintCtx { AppState* s; };

static void on_begin_print(GtkPrintOperation* op, GtkPrintContext*, gpointer user_data) {
  PrintCtx* pc = (PrintCtx*)user_data;
  gtk_print_operation_set_n_pages(op, (pc && pc->s) ? pc->s->n_pages : 0);
}

static void on_draw_print_page(GtkPrintOperation*, GtkPrintContext* ctx, gint page_nr, gpointer user_data) {
  PrintCtx* pc = (PrintCtx*)user_data;
  if (!pc || !pc->s || !pc->s->doc) return;
  PopplerPage* page = poppler_document_get_page(pc->s->doc, page_nr);
  if (!page) return;
  double pw=0, ph=0;
  poppler_page_get_size(page, &pw, &ph);
  cairo_t* cr = gtk_print_context_get_cairo_context(ctx);
  const double w = gtk_print_context_get_width(ctx);
  const double h = gtk_print_context_get_height(ctx);
  const double scale = std::min(w / pw, h / ph);
  const double dx = (w - pw * scale) * 0.5;
  const double dy = (h - ph * scale) * 0.5;
  cairo_save(cr);
  cairo_translate(cr, dx, dy);
  cairo_scale(cr, scale, scale);
  poppler_page_render_for_printing(page, cr);
  cairo_restore(cr);
  g_object_unref(page);
}

static void print_document(AppState* s) {
  if (!s || !s->doc) {
    info_box(s, "No PDF loaded.");
    return;
  }
  GtkPrintOperation* op = gtk_print_operation_new();
  gtk_print_operation_set_use_full_page(op, TRUE);
  gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);

  // Make "Current page" available in the print dialog.
  gtk_print_operation_set_current_page(op, clampi(s->current_left, 0, std::max(0, s->n_pages - 1)));

  PrintCtx pc{ s };
  g_signal_connect(op, "begin-print", G_CALLBACK(on_begin_print), &pc);
  g_signal_connect(op, "draw-page", G_CALLBACK(on_draw_print_page), &pc);
  GError* err = nullptr;
  dialog_begin(s, s->window);
  gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, GTK_WINDOW(s->window), &err);
  dialog_end(s);
  if (err) {
    info_box(s, std::string("Print failed: ") + err->message);
    g_error_free(err);
  }
  g_object_unref(op);
}

static void show_about_box(AppState* s) {
  GtkWidget* dlg = gtk_about_dialog_new();
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg), "rdScore");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), RDSCORE_VERSION);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg), "Lightweight PDF reader for musicians and general PDF use.");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(s->window));
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);
  dialog_begin(s, dlg);
  gtk_dialog_run(GTK_DIALOG(dlg));
  dialog_end(s);
  gtk_widget_destroy(dlg);
}

static void show_main_menu_dialog(AppState* s) {
  GtkWidget* dlg = gtk_dialog_new_with_buttons(
      "rdScore",
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "Open _file", 1,
      "Open _setlist", 2,
      "_Quit", GTK_RESPONSE_CLOSE,
      nullptr);
  g_signal_connect(dlg, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);
  GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget* label = gtk_label_new("Choose an action:");
  gtk_container_set_border_width(GTK_CONTAINER(area), 14);
  gtk_container_add(GTK_CONTAINER(area), label);
  gtk_widget_show_all(dlg);

  bool done = false;
  while (!done) {
    dialog_begin(s, dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    dialog_end(s);
    switch (resp) {
      case 1:
        if (choose_open_pdf(s)) done = true;
        break;
      case 2:
        if (open_setlist_dialog(s)) done = true;
        break;
      default:
        done = true;
        if (s->window) gtk_widget_destroy(s->window);
        break;
    }
  }
  gtk_widget_destroy(dlg);
}

static void on_menu_open(GtkWidget*, gpointer user_data) { choose_open_pdf((AppState*)user_data); }
static void on_menu_close(GtkWidget*, gpointer user_data) { close_current_document((AppState*)user_data); }
static void on_menu_print(GtkWidget*, gpointer user_data) { print_document((AppState*)user_data); }
static void on_menu_setlist(GtkWidget*, gpointer user_data) { open_setlist_dialog((AppState*)user_data); }
static void on_menu_create_setlist(GtkWidget*, gpointer user_data) { create_setlist_dialog((AppState*)user_data); }
static void on_menu_edit_setlist(GtkWidget*, gpointer user_data) { edit_setlist_dialog((AppState*)user_data); }
static void on_menu_rename_setlist(GtkWidget*, gpointer user_data) { rename_setlist_dialog((AppState*)user_data); }
static void on_menu_delete_setlist(GtkWidget*, gpointer user_data) { delete_setlist_dialog((AppState*)user_data); }
static void on_menu_help(GtkWidget*, gpointer user_data) { show_help((AppState*)user_data); }
static void on_menu_about(GtkWidget*, gpointer user_data) { show_about_box((AppState*)user_data); }
static void on_menu_quit(GtkWidget*, gpointer) { gtk_main_quit(); }

static GtkWidget* build_menu_bar(AppState* s) {
  GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* menubar = gtk_menu_bar_new();
  gtk_widget_set_can_focus(menubar, TRUE);

  GtkWidget* file_item = gtk_menu_item_new_with_mnemonic("_File");
  GtkWidget* file_menu = gtk_menu_new();
  GtkWidget* mi_open = gtk_menu_item_new_with_mnemonic("_Open");
  GtkWidget* mi_close = gtk_menu_item_new_with_mnemonic("_Close");
  GtkWidget* mi_print = gtk_menu_item_new_with_mnemonic("_Print");
  GtkWidget* mi_quit = gtk_menu_item_new_with_mnemonic("_Quit");
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_open);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_close);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_print);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_quit);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);

  GtkWidget* setlists_item = gtk_menu_item_new_with_mnemonic("_Setlists");
  GtkWidget* setlists_menu = gtk_menu_new();
  GtkWidget* mi_open_setlist = gtk_menu_item_new_with_mnemonic("_Open setlist");
  GtkWidget* mi_create_setlist = gtk_menu_item_new_with_mnemonic("_Create setlist");
  GtkWidget* mi_edit_setlist = gtk_menu_item_new_with_mnemonic("_Edit setlist");
  GtkWidget* mi_rename_setlist = gtk_menu_item_new_with_mnemonic("_Rename setlist");
  GtkWidget* mi_delete_setlist = gtk_menu_item_new_with_mnemonic("_Delete setlist");
  gtk_menu_shell_append(GTK_MENU_SHELL(setlists_menu), mi_open_setlist);
  gtk_menu_shell_append(GTK_MENU_SHELL(setlists_menu), mi_create_setlist);
  gtk_menu_shell_append(GTK_MENU_SHELL(setlists_menu), mi_edit_setlist);
  gtk_menu_shell_append(GTK_MENU_SHELL(setlists_menu), mi_rename_setlist);
  gtk_menu_shell_append(GTK_MENU_SHELL(setlists_menu), mi_delete_setlist);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(setlists_item), setlists_menu);

  GtkWidget* help_item = gtk_menu_item_new_with_mnemonic("_Help");
  GtkWidget* help_menu = gtk_menu_new();
  GtkWidget* mi_help = gtk_menu_item_new_with_mnemonic("_Help");
  GtkWidget* mi_about = gtk_menu_item_new_with_mnemonic("_About");
  gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), mi_help);
  gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), mi_about);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);

  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), setlists_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);

  GtkWidget* spacer = gtk_label_new(nullptr);
  gtk_widget_set_hexpand(spacer, TRUE);
  GtkWidget* status = gtk_label_new("No PDF | Zoom 100%");
  gtk_widget_set_halign(status, GTK_ALIGN_END);
  gtk_widget_set_margin_end(status, 8);
  gtk_widget_set_margin_start(status, 8);

  gtk_box_pack_start(GTK_BOX(hbox), menubar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), spacer, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), status, FALSE, FALSE, 0);

  if (s) {
    s->menubar = menubar;
    s->menu_file = file_item;
    s->menu_setlists = setlists_item;
    s->menu_help = help_item;
    s->status_label = status;
  }

  g_signal_connect(mi_open, "activate", G_CALLBACK(on_menu_open), s);
  g_signal_connect(mi_close, "activate", G_CALLBACK(on_menu_close), s);
  g_signal_connect(mi_print, "activate", G_CALLBACK(on_menu_print), s);
  g_signal_connect(mi_quit, "activate", G_CALLBACK(on_menu_quit), s);
  g_signal_connect(mi_open_setlist, "activate", G_CALLBACK(on_menu_setlist), s);
  g_signal_connect(mi_create_setlist, "activate", G_CALLBACK(on_menu_create_setlist), s);
  g_signal_connect(mi_edit_setlist, "activate", G_CALLBACK(on_menu_edit_setlist), s);
  g_signal_connect(mi_rename_setlist, "activate", G_CALLBACK(on_menu_rename_setlist), s);
  g_signal_connect(mi_delete_setlist, "activate", G_CALLBACK(on_menu_delete_setlist), s);
  g_signal_connect(mi_help, "activate", G_CALLBACK(on_menu_help), s);
  g_signal_connect(mi_about, "activate", G_CALLBACK(on_menu_about), s);

  return hbox;
}

int main(int argc, char** argv) {
  gtk_init(&argc, &argv);

  ensure_setlists_directory();

  if (argc == 2 && std::string(argv[1]) == "--version") {
    g_print("rdScore %s\n", RDSCORE_VERSION);
    return 0;
  }

  AppState s;
  update_zoom_percent(&s);

  s.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(s.window), 1200, 800);
  gtk_window_set_title(GTK_WINDOW(s.window), "rdScore");

  GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(s.window), vbox);

  GtkWidget* menubar = build_menu_bar(&s);
  gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
  update_status_label(&s);

  s.scrolled = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s.scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(vbox), s.scrolled, TRUE, TRUE, 0);

  s.drawing = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(s.scrolled), s.drawing);

  g_signal_connect(s.window, "destroy", G_CALLBACK(on_main_window_destroy), &s);
  g_signal_connect(s.window, "key-press-event", G_CALLBACK(on_key), &s);
  g_signal_connect(s.window, "realize", G_CALLBACK(on_realize), &s);
  g_signal_connect(s.scrolled, "size-allocate", G_CALLBACK(on_size_allocate), &s);
  g_signal_connect(s.drawing, "draw", G_CALLBACK(on_draw), &s);

  gtk_widget_show_all(s.window);

  if (argc >= 2) {
    if (!load_document_from_path(&s, argv[1], true)) {
      unload_document(&s);
    }
  }

  gtk_main();

  if (s.zoom_overlay_timer) {
    g_source_remove(s.zoom_overlay_timer);
    s.zoom_overlay_timer = 0;
  }
  if (s.page_overlay_timer) {
    g_source_remove(s.page_overlay_timer);
    s.page_overlay_timer = 0;
  }

  unload_document(&s);
  return 0;
}
