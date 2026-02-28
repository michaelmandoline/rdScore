// rdScore.cpp (v1.0.4 - Concert + 2 pages + Zoom + Scrollbars + Help page + Zoom overlay + Smart advance + Extract)
// Build:
//   g++ -O2 -std=c++17 rdScore.cpp -o rdScore $(pkg-config --cflags --libs gtk+-3.0 poppler-glib)

#include <gtk/gtk.h>
#include <poppler.h>

#include <algorithm>
#include <string>
#include <cmath>

static const char* RDSCORE_VERSION = "1.0.4";

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

  GtkWidget* window = nullptr;
  GtkWidget* scrolled = nullptr;
  GtkWidget* drawing = nullptr;

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

static void normalize_left(AppState* s) {
  s->current_left = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
  if (s->two_pages && (s->current_left % 2 == 1)) s->current_left--;
  s->current_left = clampi(s->current_left, 0, std::max(0, s->n_pages - 1));
}

static void hide_cursor(GtkWidget* widget) {
  GdkWindow* gw = gtk_widget_get_window(widget);
  if (!gw) return;
  GdkDisplay* dpy = gdk_window_get_display(gw);
  GdkCursor* cur = gdk_cursor_new_for_display(dpy, GDK_BLANK_CURSOR);
  gdk_window_set_cursor(gw, cur);
  g_object_unref(cur);
}

static void show_cursor(GtkWidget* widget) {
  GdkWindow* gw = gtk_widget_get_window(widget);
  if (!gw) return;
  gdk_window_set_cursor(gw, nullptr);
}

static void toggle_fullscreen(AppState* s) {
  s->fullscreen = !s->fullscreen;
  if (s->fullscreen) {
    gtk_window_fullscreen(GTK_WINDOW(s->window));
    hide_cursor(s->window);
  } else {
    gtk_window_unfullscreen(GTK_WINDOW(s->window));
    show_cursor(s->window);
    // When leaving fullscreen, GTK may keep the previous scroll offset until the next
    // size allocation / redraw. Request a recenter so the content is immediately placed.
    s->pending_recenter = true;
  }
}

static void queue_redraw(AppState* s) {
  if (s && s->drawing) gtk_widget_queue_draw(s->drawing);
}

// ===== Helper: ESC closes dialogs reliably
static gboolean dialog_esc_to_cancel(GtkWidget* widget, GdkEventKey* ev, gpointer) {
  if (ev && ev->keyval == GDK_KEY_Escape) {
    gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_CANCEL);
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
  trigger_page_overlay(s);
  queue_redraw(s);
}
static void zoom_out(AppState* s) {
  s->zoom = clampd(s->zoom / 1.10, 0.30, 5.00);
  update_zoom_percent(s);
  trigger_zoom_overlay(s);
  compute_content_size(s);
  trigger_page_overlay(s);
  queue_redraw(s);
}
static void zoom_reset(AppState* s){
  s->zoom = 1.0;
  update_zoom_percent(s);
  trigger_zoom_overlay(s);
  compute_content_size(s);
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

  int resp = gtk_dialog_run(GTK_DIALOG(dialog));
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

  gtk_dialog_run(GTK_DIALOG(d));
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
  if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
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

  int p1 = 0, p2 = 0;
  if (!ask_int(s, "Extraction PDF", "Page début (1..N) :", p1)) return;
  if (!ask_int(s, "Extraction PDF", "Page fin (1..N) :", p2)) return;

  if (p1 < 1 || p2 < 1 || p1 > s->n_pages || p2 > s->n_pages) {
    info_box(s, "Pages hors limites.\nRappel: 1 <= début <= fin <= " + std::to_string(s->n_pages));
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

  // Force .pdf extension if missing
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
      "  Esc   : quitter";

  GtkWidget* d = gtk_message_dialog_new(
      GTK_WINDOW(s->window),
      (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_INFO,
      GTK_BUTTONS_CLOSE,
      "%s",
      help.c_str());

  g_signal_connect(d, "key-press-event", G_CALLBACK(dialog_esc_to_cancel), nullptr);

  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

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

  int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  if (resp == GTK_RESPONSE_OK) {
    const char* txt = gtk_entry_get_text(GTK_ENTRY(entry));
    int page = txt ? atoi(txt) : 0;
    if (page >= 1 && page <= s->n_pages) goto_left_page(s, page - 1);
  }
  gtk_widget_destroy(dialog);
}

static gboolean on_key(GtkWidget*, GdkEventKey* ev, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (!s) return FALSE;

  const bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;

  // Zoom
  if (ctrl) {
    switch (ev->keyval) {
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
      gtk_main_quit();
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
      s->two_pages = false;
      normalize_left(s);
      compute_content_size(s);
      trigger_page_overlay(s);
      queue_redraw(s);
      return TRUE;

    case GDK_KEY_2:
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
  if (!s || !s->doc || s->n_pages <= 0) return FALSE;

  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  const int W = std::max(1, a.width);
  const int H = std::max(1, a.height);

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

  // background (concert)
  cairo_save(cr);
  cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
  cairo_paint(cr);
  cairo_restore(cr);

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
    std::string z = std::to_string(s->zoom_overlay_percent) + " %";

    const double bw = 200.0, bh = 96.0;
    const double x = (W - bw) * 0.5;
    const double y = (H - bh) * 0.5;

    cairo_save(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.60);
    cairo_rectangle(cr, x, y, bw, bh);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 36);

    cairo_text_extents_t te;
    cairo_text_extents(cr, z.c_str(), &te);
    const double tx = x + (bw - te.width) * 0.5 - te.x_bearing;
    const double ty = y + (bh - te.height) * 0.5 - te.y_bearing;

    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, z.c_str());

    cairo_restore(cr);
  }


  if (s->page_overlay) {
    const std::string& t = s->page_overlay_text;

    const double pad = 18.0;
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 22);

    cairo_text_extents_t te;
    cairo_text_extents(cr, t.c_str(), &te);

    const double bw = te.width + pad * 2.0;
    const double bh = te.height + pad * 1.6;

    const double x = (W - bw) * 0.5;
    const double y = std::max(12.0, H * 0.08);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
    cairo_rectangle(cr, x, y, bw, bh);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
    const double tx = x + pad - te.x_bearing;
    const double ty = y + (bh - te.height) * 0.5 - te.y_bearing;

    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, t.c_str());

    cairo_restore(cr);
  }


  return FALSE;
}

// Apply fullscreen only after realize
static void on_realize(GtkWidget* widget, gpointer user_data) {
  AppState* s = (AppState*)user_data;
  if (s && s->fullscreen) {
    gtk_window_fullscreen(GTK_WINDOW(widget));
    hide_cursor(widget);
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

int main(int argc, char** argv) {
  gtk_init(&argc, &argv);

  // --version option
  if (argc == 2 && std::string(argv[1]) == "--version") {
    g_print("rdScore %s\n", RDSCORE_VERSION);
    return 0;
  }

  if (argc < 2) {
    g_printerr("Usage: rdScore fichier.pdf\n");
    return 1;
  }

  AppState s;
  update_zoom_percent(&s);

  // Robust: absolute path before g_filename_to_uri
  char* abs_path = g_canonicalize_filename(argv[1], nullptr);
  s.input_pdf_abs = abs_path ? abs_path : "";
  char* uri = g_filename_to_uri(abs_path, nullptr, nullptr);
  g_free(abs_path);

  if (!uri) {
    g_printerr("Bad path.\n");
    return 1;
  }

  GError* err = nullptr;
  s.doc = poppler_document_new_from_file(uri, nullptr, &err);
  g_free(uri);

  if (!s.doc) {
    g_printerr("Impossible d'ouvrir le PDF: %s\n", err ? err->message : "unknown error");
    if (err) g_error_free(err);
    return 1;
  }

  s.n_pages = poppler_document_get_n_pages(s.doc);
  if (s.n_pages <= 0) {
    g_printerr("PDF sans pages.\n");
    g_object_unref(s.doc);
    return 1;
  }

  normalize_left(&s);

  s.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(s.window), 1200, 800);

  s.scrolled = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s.scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(s.window), s.scrolled);

  s.drawing = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(s.scrolled), s.drawing);

  g_signal_connect(s.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  g_signal_connect(s.window, "key-press-event", G_CALLBACK(on_key), &s);
  g_signal_connect(s.window, "realize", G_CALLBACK(on_realize), &s);
  g_signal_connect(s.scrolled, "size-allocate", G_CALLBACK(on_size_allocate), &s);
  g_signal_connect(s.drawing, "draw", G_CALLBACK(on_draw), &s);

  gtk_widget_show_all(s.window);

  gtk_main();

  // Clean overlay timer if still armed
  if (s.zoom_overlay_timer) {
    g_source_remove(s.zoom_overlay_timer);
    s.zoom_overlay_timer = 0;
  }

  g_object_unref(s.doc);
  return 0;
}
