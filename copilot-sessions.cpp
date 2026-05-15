// ============================================================================
// copilot-sessions
// Terminal UI for browsing and resuming GitHub Copilot CLI sessions.
//
// Author: Daniel Petrovic (daniel-dev@hotmail.de)
// Year: 2026
// License: MIT
// ============================================================================

#include <notcurses/notcurses.h>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "dev"
#endif

struct Rgb {
  unsigned r;
  unsigned g;
  unsigned b;
};

struct Theme {
  Rgb chrome_fg{186, 194, 222};
  Rgb base_bg{10, 14, 26};
  Rgb panel_bg{18, 24, 42};
  Rgb panel_alt_bg{12, 18, 32};
  Rgb accent{108, 92, 231};
  Rgb accent_2{0, 206, 201};
  Rgb good{46, 204, 113};
  Rgb warm{253, 203, 110};
  Rgb user{116, 185, 255};
  Rgb dim{99, 110, 136};
  Rgb alert{255, 118, 117};
};

struct SessionEntry {
  std::string id;
  std::string summary;
  std::string created_at;
  std::string updated_at;
  std::string cwd;
  std::string repository;
};

struct CwdFilter {
  std::string label;
  std::string value;
  bool is_all = false;
  bool exists = true;
};

struct SessionTurn {
  int turn_index = 0;
  std::string user_message;
  std::string assistant_response;
  std::string timestamp;
};

struct SessionCheckpoint {
  int checkpoint_number = 0;
  std::string title;
  std::string overview;
  std::string work_done;
  std::string next_steps;
};

struct SessionDetail {
  bool loaded = false;
  std::string id;
  std::string summary;
  std::string created_at;
  std::string updated_at;
  std::string cwd;
  std::string repository;
  std::string branch;
  std::string host_type;
  int turn_count = 0;
  int checkpoint_count = 0;
  int file_count = 0;
  int ref_count = 0;
  std::vector<std::string> refs;
  std::vector<std::string> files;
  std::vector<SessionTurn> turns;
  std::vector<SessionCheckpoint> checkpoints;
};

enum class FocusPane {
  Cwds,
  Sessions,
};

enum class ThemeMode {
  Dark,
  Light,
};

enum class SessionSort {
  UpdatedDesc,
  UpdatedAsc,
  CreatedDesc,
  CreatedAsc,
};

enum class ModalKind {
  SessionDetail,
  Help,
};

struct AppState {
  std::vector<SessionEntry> all_sessions;
  std::vector<SessionEntry> sessions;
  std::vector<CwdFilter> cwd_filters;
  SessionDetail detail;
  std::string db_path;
  std::string db_size_label = "(unknown)";
  std::string status = "Ready";
  int selected_index = 0;
  int scroll_offset = 0;
  int selected_cwd_index = 0;
  int cwd_scroll_offset = 0;
  bool modal_open = false;
  ModalKind modal_kind = ModalKind::SessionDetail;
  int modal_scroll_offset = 0;
  bool modal_g_pending = false;
  bool browser_g_pending = false;
  bool resume_warning_open = false;
  bool clipboard_modal_open = false;
  bool show_selected_cwd_path = false;
  FocusPane focus = FocusPane::Sessions;
  ThemeMode theme_mode = ThemeMode::Dark;
  SessionSort sort_mode = SessionSort::UpdatedDesc;
  bool sort_s_pending = false;
  bool sort_direction_pending = false;
  SessionSort pending_sort_mode = SessionSort::UpdatedDesc;
  bool command_mode = false;
  std::string command_buffer = ":";
  bool resume_requested = false;
  std::string resume_session_id;
  std::string resume_session_cwd;
  std::string pending_resume_session_id;
  std::string pending_resume_session_cwd;
  std::string clipboard_session_id;
};

void set_colors(ncplane *plane, const Rgb &fg, const Rgb &bg,
                unsigned styles = NCSTYLE_NONE) {
  ncplane_set_fg_rgb8(plane, fg.r, fg.g, fg.b);
  ncplane_set_bg_rgb8(plane, bg.r, bg.g, bg.b);
  ncplane_set_styles(plane, styles);
}

void reset_colors(ncplane *plane) {
  ncplane_set_fg_default(plane);
  ncplane_set_bg_default(plane);
  ncplane_set_styles(plane, NCSTYLE_NONE);
}

void fill_rect(ncplane *plane, int y, int x, int height, int width,
               const Rgb &bg) {
  set_colors(plane, bg, bg);
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      ncplane_putchar_yx(plane, y + row, x + col, ' ');
    }
  }
  reset_colors(plane);
}

void draw_hline(ncplane *plane, int y, int x, int width, char ch) {
  for (int i = 0; i < width; ++i) {
    ncplane_putchar_yx(plane, y, x + i, ch);
  }
}

void draw_vline(ncplane *plane, int y, int x, int height, char ch) {
  for (int i = 0; i < height; ++i) {
    ncplane_putchar_yx(plane, y + i, x, ch);
  }
}

void draw_box(ncplane *plane, int y, int x, int height, int width,
              const std::string &title, const Theme &theme, const Rgb &bg,
              const Rgb &border, bool highlight = false) {
  if (height < 2 || width < 2) {
    return;
  }

  fill_rect(plane, y, x, height, width, bg);

  set_colors(plane, border, bg, NCSTYLE_BOLD);
  ncplane_putchar_yx(plane, y, x, '+');
  ncplane_putchar_yx(plane, y, x + width - 1, '+');
  ncplane_putchar_yx(plane, y + height - 1, x, '+');
  ncplane_putchar_yx(plane, y + height - 1, x + width - 1, '+');
  draw_hline(plane, y, x + 1, width - 2, '=');
  draw_hline(plane, y + height - 1, x + 1, width - 2, '-');
  draw_vline(plane, y + 1, x, height - 2, '|');
  draw_vline(plane, y + 1, x + width - 1, height - 2, '|');

  if (!title.empty() && width > 4) {
    const int title_width = std::min(width - 4, static_cast<int>(title.size()));
    set_colors(plane, highlight ? theme.accent_2 : theme.warm, bg,
               NCSTYLE_BOLD);
    ncplane_printf_yx(plane, y, x + 2, "%.*s", title_width, title.c_str());
  }

  reset_colors(plane);
}

std::string ellipsize(const std::string &text, int width);

std::string normalize_timestamp_display(std::string value) {
  std::replace(value.begin(), value.end(), 'T', ' ');
  if (!value.empty() && value.back() == 'Z') {
    value.pop_back();
  }
  return value;
}

std::string format_list_timestamp(const std::string &value, int width) {
  if (width <= 0) {
    return "";
  }

  const std::string normalized = normalize_timestamp_display(value);
  if (normalized.empty()) {
    return "-";
  }
  if (width >= 16 && static_cast<int>(normalized.size()) >= 16) {
    return normalized.substr(0, 16);
  }
  if (width >= 10 && static_cast<int>(normalized.size()) >= 10) {
    return normalized.substr(0, 10);
  }
  if (width >= 8 && static_cast<int>(normalized.size()) >= 13) {
    return normalized.substr(5, 8);
  }
  if (width >= 5 && static_cast<int>(normalized.size()) >= 10) {
    return normalized.substr(5, 5);
  }
  if (width >= 4 && static_cast<int>(normalized.size()) >= 10) {
    return normalized.substr(5, 2) + normalized.substr(8, 2);
  }
  return ellipsize(normalized, width);
}

std::string shorten_id(const std::string &id) {
  if (id.size() <= 8) {
    return id;
  }
  return id.substr(0, 8);
}

std::string trim_copy(const std::string &value) {
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }

  std::size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
    --last;
  }

  return value.substr(first, last - first);
}

std::string ascii_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string expand_user_path(const std::string &path) {
  if (path.empty() || path.front() != '~') {
    return path;
  }

  const char *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return path;
  }

  if (path.size() == 1) {
    return home;
  }
  if (path[1] == '/') {
    return std::string(home) + path.substr(1);
  }
  return path;
}

std::string normalize_path(const std::string &path) {
  const std::string expanded = expand_user_path(trim_copy(path));
  if (expanded.empty()) {
    return expanded;
  }

  fs::path normalized(expanded);
  if (normalized.is_relative()) {
    std::error_code error;
    normalized = fs::absolute(normalized, error);
    if (error) {
      return expanded;
    }
  }

  return normalized.lexically_normal().string();
}

std::string configured_db_path(const AppState &state) {
  return state.db_path.empty() ? "(not loaded)" : state.db_path;
}

std::string project_version() { return PROJECT_VERSION; }

std::string default_db_path() {
  const char *copilot_home = std::getenv("COPILOT_HOME");
  if (copilot_home != nullptr && *copilot_home != '\0') {
    return normalize_path(std::string(copilot_home) + "/session-store.db");
  }

  const char *home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return normalize_path(std::string(home) + "/.copilot/session-store.db");
  }

  return "";
}

Theme dark_theme() { return Theme{}; }

Theme light_theme() {
  Theme theme;
  theme.chrome_fg = {36, 41, 46};
  theme.base_bg = {244, 247, 252};
  theme.panel_bg = {229, 234, 242};
  theme.panel_alt_bg = {220, 226, 236};
  theme.accent = {33, 94, 196};
  theme.accent_2 = {0, 121, 107};
  theme.good = {46, 125, 50};
  theme.warm = {230, 126, 34};
  theme.user = {21, 101, 192};
  theme.dim = {102, 112, 133};
  theme.alert = {198, 40, 40};
  return theme;
}

const char *theme_mode_label(ThemeMode mode) {
  return mode == ThemeMode::Dark ? "dark" : "light";
}

const char *session_sort_label(SessionSort mode) {
  switch (mode) {
  case SessionSort::UpdatedDesc:
    return "last update desc";
  case SessionSort::UpdatedAsc:
    return "last update asc";
  case SessionSort::CreatedDesc:
    return "creation time desc";
  case SessionSort::CreatedAsc:
    return "creation time asc";
  }
  return "last update desc";
}

const char *session_sort_key_label(SessionSort mode) {
  switch (mode) {
  case SessionSort::UpdatedDesc:
    return "sud";
  case SessionSort::UpdatedAsc:
    return "sua";
  case SessionSort::CreatedDesc:
    return "scd";
  case SessionSort::CreatedAsc:
    return "sca";
  }
  return "sud";
}

Theme current_theme(const AppState &state) {
  return state.theme_mode == ThemeMode::Dark ? dark_theme() : light_theme();
}

const SessionEntry *current_session(const AppState &state) {
  if (state.sessions.empty() || state.selected_index < 0 ||
      state.selected_index >= static_cast<int>(state.sessions.size())) {
    return nullptr;
  }
  return &state.sessions[state.selected_index];
}

bool command_available(const char *name) {
  if (name == nullptr || *name == '\0') {
    return false;
  }

  const char *path = std::getenv("PATH");
  if (path == nullptr) {
    return false;
  }

  std::string path_value(path);
  std::size_t start = 0;
  while (start <= path_value.size()) {
    const std::size_t end = path_value.find(':', start);
    std::string dir = path_value.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (dir.empty()) {
      dir = ".";
    }

    const std::string candidate = dir + "/" + name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return true;
    }

    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return false;
}

bool directory_exists(const std::string &path) {
  if (path.empty()) {
    return true;
  }

  std::error_code error;
  return fs::is_directory(path, error);
}

bool pipe_text_to_command(const char *command, const std::string &text) {
  if (command == nullptr || *command == '\0') {
    return false;
  }

  FILE *pipe = popen(command, "w");
  if (pipe == nullptr) {
    return false;
  }

  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t written =
        fwrite(text.data() + offset, 1, text.size() - offset, pipe);
    if (written == 0) {
      pclose(pipe);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }

  return pclose(pipe) == 0;
}

bool copy_text_to_clipboard(const std::string &text) {
  if (command_available("wl-copy") && pipe_text_to_command("wl-copy", text)) {
    return true;
  }
  if (command_available("xclip") &&
      pipe_text_to_command("xclip -selection clipboard", text)) {
    return true;
  }
  if (command_available("xsel") &&
      pipe_text_to_command("xsel --clipboard --input", text)) {
    return true;
  }
  if (command_available("pbcopy") && pipe_text_to_command("pbcopy", text)) {
    return true;
  }
  return false;
}

void close_clipboard_modal(AppState &state, const std::string &status) {
  state.clipboard_modal_open = false;
  state.clipboard_session_id.clear();
  state.status = status;
}

bool yank_selected_session_id(AppState &state) {
  if (state.focus != FocusPane::Sessions) {
    state.status = "Focus the session list to yank a session ID.";
    return false;
  }

  const SessionEntry *selected = current_session(state);
  if (selected == nullptr) {
    state.status = "No session selected.";
    return false;
  }
  if (selected->id.empty()) {
    state.status = "Selected session has no ID to copy.";
    return false;
  }
  if (!copy_text_to_clipboard(selected->id)) {
    state.status = "Clipboard copy failed: no supported clipboard tool found.";
    return false;
  }

  state.clipboard_modal_open = true;
  state.clipboard_session_id = selected->id;
  state.status = "Copied session ID to clipboard.";
  return true;
}

// Queue the selected session to be resumed in Copilot CLI after the TUI exits.
bool queue_resume_request(AppState &state, const std::string &session_id,
                          const std::string &session_cwd) {
  if (session_id.empty()) {
    state.status = "Selected session has no ID to resume.";
    return false;
  }
  if (!command_available("copilot")) {
    state.status = "copilot command is not available on PATH.";
    return false;
  }

  state.resume_requested = true;
  state.resume_session_id = session_id;
  state.resume_session_cwd = session_cwd;
  state.status =
      "Continuing session " + shorten_id(session_id) + " in Copilot CLI...";
  return true;
}

bool queue_resume_selected_session(AppState &state) {
  const SessionEntry *selected = current_session(state);
  if (selected == nullptr) {
    state.status = "No session selected.";
    return false;
  }
  if (!directory_exists(selected->cwd)) {
    state.resume_warning_open = true;
    state.pending_resume_session_id = selected->id;
    state.pending_resume_session_cwd = selected->cwd;
    state.status = "Selected session folder is missing.";
    return false;
  }

  return queue_resume_request(state, selected->id, selected->cwd);
}

void close_resume_warning(AppState &state, const std::string &status) {
  state.resume_warning_open = false;
  state.pending_resume_session_id.clear();
  state.pending_resume_session_cwd.clear();
  state.status = status;
}

void focus_pane(AppState &state, FocusPane pane) {
  if (state.focus == pane) {
    return;
  }

  state.focus = pane;
  if (pane == FocusPane::Cwds) {
    state.show_selected_cwd_path = true;
    state.status = "Focused CWD filters. Full path mode enabled.";
    return;
  }

  state.status = "Focused session list.";
}

void open_command_mode(AppState &state) {
  state.command_mode = true;
  state.command_buffer = ":";
  state.status =
      "Command mode. Try :help, :theme dark, :theme light, or :open <db path>.";
}

void close_command_mode(AppState &state, const std::string &status) {
  state.command_mode = false;
  state.command_buffer = ":";
  state.status = status;
}

bool load_sessions(AppState &state);

void open_help_modal(AppState &state) {
  state.modal_open = true;
  state.modal_kind = ModalKind::Help;
  state.modal_scroll_offset = 0;
  state.modal_g_pending = false;
  state.status = "Opened help modal.";
}

void execute_command(AppState &state, const std::string &command) {
  std::string text = trim_copy(command);
  if (!text.empty() && text.front() == ':') {
    text.erase(text.begin());
  }
  text = trim_copy(text);
  if (text.empty()) {
    state.status = "Command canceled.";
    return;
  }

  std::istringstream stream(text);
  std::string name;
  stream >> name;
  name = ascii_lower_copy(name);
  std::string arg;
  std::getline(stream, arg);
  arg = trim_copy(arg);

  if (name == "theme") {
    const std::string theme_name = ascii_lower_copy(arg);
    if (theme_name == "dark") {
      if (state.theme_mode == ThemeMode::Dark) {
        state.status = "Theme already set to dark.";
      } else {
        state.theme_mode = ThemeMode::Dark;
        state.status = "Switched to dark theme.";
      }
      return;
    }
    if (theme_name == "light") {
      if (state.theme_mode == ThemeMode::Light) {
        state.status = "Theme already set to light.";
      } else {
        state.theme_mode = ThemeMode::Light;
        state.status = "Switched to light theme.";
      }
      return;
    }
    state.status = "Unknown theme. Use :theme dark or :theme light.";
    return;
  }

  if (name == "help" && arg.empty()) {
    open_help_modal(state);
    return;
  }

  if (name == "open") {
    if (arg.empty()) {
      state.status = "Missing database path. Use :open <db path>.";
      return;
    }

    AppState next_state = state;
    next_state.db_path = normalize_path(arg);
    if (!load_sessions(next_state)) {
      state.status = next_state.status;
      return;
    }

    state = std::move(next_state);
    state.status = "Opened " + state.db_path + " (" +
                   std::to_string(state.all_sessions.size()) + " sessions).";
    return;
  }

  state.status =
      "Unknown command. Use :help, :theme dark, :theme light, or :open <db path>.";
}

bool handle_command_key(uint32_t key, AppState &state) {
  switch (key) {
  case NCKEY_ENTER:
  case '\n':
  case '\r': {
    const std::string command = state.command_buffer;
    state.command_mode = false;
    state.command_buffer = ":";
    execute_command(state, command);
    return true;
  }
  case 27:
    close_command_mode(state, "Command canceled.");
    return true;
  case NCKEY_BACKSPACE:
  case 127:
  case '\b':
    if (state.command_buffer.size() <= 1) {
      close_command_mode(state, "Command canceled.");
    } else {
      state.command_buffer.pop_back();
      state.status = "Editing command.";
    }
    return true;
  default:
    if (key <= 0xff &&
        std::isprint(static_cast<unsigned char>(key)) != 0) {
      state.command_buffer.push_back(static_cast<char>(key));
      state.status = "Editing command.";
    }
    return true;
  }
}

int resume_in_copilot(const AppState &state) {
  if (!state.resume_requested) {
    return 0;
  }

  if (!state.resume_session_cwd.empty() &&
      chdir(state.resume_session_cwd.c_str()) != 0) {
    std::fprintf(stderr,
                 "failed to change directory to stored session cwd %s: %s\n",
                 state.resume_session_cwd.c_str(), std::strerror(errno));
    return 1;
  }

  std::vector<std::string> args{
      "copilot",
      "--resume=" + state.resume_session_id,
  };
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  execvp(argv.front(), argv.data());
  std::fprintf(stderr, "failed to launch copilot: %s\n", std::strerror(errno));
  return 1;
}

std::string ellipsize(const std::string &text, int width) {
  if (width <= 0) {
    return "";
  }
  if (static_cast<int>(text.size()) <= width) {
    return text;
  }
  if (width <= 3) {
    return text.substr(0, width);
  }
  return text.substr(0, width - 3) + "...";
}

std::string sort_primary_timestamp(const SessionEntry &session,
                                   SessionSort mode) {
  if (mode == SessionSort::UpdatedDesc || mode == SessionSort::UpdatedAsc) {
    return session.updated_at.empty() ? session.created_at : session.updated_at;
  }
  return session.created_at;
}

void sort_sessions(std::vector<SessionEntry> &sessions, SessionSort mode) {
  const bool descending =
      mode == SessionSort::UpdatedDesc || mode == SessionSort::CreatedDesc;
  std::sort(sessions.begin(), sessions.end(),
            [mode, descending](const SessionEntry &lhs, const SessionEntry &rhs) {
              const std::string lhs_primary = sort_primary_timestamp(lhs, mode);
              const std::string rhs_primary = sort_primary_timestamp(rhs, mode);
              if (lhs_primary != rhs_primary) {
                return descending ? lhs_primary > rhs_primary
                                  : lhs_primary < rhs_primary;
              }
              if (lhs.updated_at != rhs.updated_at) {
                return descending ? lhs.updated_at > rhs.updated_at
                                  : lhs.updated_at < rhs.updated_at;
              }
              if (lhs.created_at != rhs.created_at) {
                return descending ? lhs.created_at > rhs.created_at
                                  : lhs.created_at < rhs.created_at;
              }
              return lhs.id < rhs.id;
            });
}

std::string format_bytes(long long bytes) {
  if (bytes < 0) {
    return "(unknown)";
  }

  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }

  char buffer[32];
  if (unit == 0) {
    std::snprintf(buffer, sizeof(buffer), "%lld %s", bytes, units[unit]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
  }
  return buffer;
}

std::vector<std::string> wrap_text(const std::string &text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) {
    return lines;
  }

  std::string current;
  for (char ch : text) {
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
    if (static_cast<int>(current.size()) >= width) {
      lines.push_back(current);
      current.clear();
    }
  }
  if (!current.empty() || lines.empty()) {
    lines.push_back(current);
  }
  return lines;
}

int session_visible_rows(int rows) {
  constexpr int header_h = 4;
  constexpr int detail_h = 8;
  constexpr int footer_h = 3;
  constexpr int session_header_lines = 6;
  return std::max(1,
                  rows - header_h - detail_h - footer_h - session_header_lines);
}

int cwd_visible_rows(int rows) {
  constexpr int header_h = 4;
  constexpr int detail_h = 8;
  constexpr int footer_h = 3;
  constexpr int cwd_header_lines = 3;
  return std::max(1, rows - header_h - detail_h - footer_h - cwd_header_lines);
}

int modal_visible_rows(int rows) {
  return std::max(1, std::min(rows - 4, 18) - 4);
}

const char *column_text(sqlite3_stmt *stmt, int column) {
  const auto *value = sqlite3_column_text(stmt, column);
  return value != nullptr ? reinterpret_cast<const char *>(value) : "";
}

void append_wrapped_block(std::vector<std::string> &lines,
                          const std::string &prefix, const std::string &text,
                          int width) {
  const auto wrapped =
      wrap_text(text.empty() ? "(none)" : text,
                std::max(1, width - static_cast<int>(prefix.size())));
  if (wrapped.empty()) {
    lines.push_back(prefix);
    return;
  }
  lines.push_back(prefix + wrapped.front());
  for (size_t i = 1; i < wrapped.size(); ++i) {
    lines.push_back(std::string(prefix.size(), ' ') + wrapped[i]);
  }
}

std::vector<std::string> build_modal_lines(const SessionDetail &detail,
                                           int width) {
  std::vector<std::string> lines;
  const int content_width = std::max(12, width);

  append_wrapped_block(lines, "ID        ", detail.id, content_width);
  append_wrapped_block(lines, "SUMMARY   ",
                       detail.summary.empty() ? "(no summary)" : detail.summary,
                       content_width);
  append_wrapped_block(lines, "CREATED   ", detail.created_at, content_width);
  append_wrapped_block(lines, "UPDATED   ", detail.updated_at, content_width);
  append_wrapped_block(lines, "FOLDER    ",
                       detail.cwd.empty() ? "(none)" : detail.cwd,
                       content_width);
  append_wrapped_block(lines, "REPO      ",
                       detail.repository.empty() ? "(none)" : detail.repository,
                       content_width);
  append_wrapped_block(lines, "BRANCH    ",
                       detail.branch.empty() ? "(none)" : detail.branch,
                       content_width);
  append_wrapped_block(lines, "HOST      ",
                       detail.host_type.empty() ? "(none)" : detail.host_type,
                       content_width);
  lines.push_back("COUNTS    turns=" + std::to_string(detail.turn_count) +
                  " checkpoints=" + std::to_string(detail.checkpoint_count) +
                  " files=" + std::to_string(detail.file_count) +
                  " refs=" + std::to_string(detail.ref_count));

  lines.push_back("");
  lines.push_back("Refs");
  if (detail.refs.empty()) {
    lines.push_back("  (none)");
  } else {
    for (const auto &ref : detail.refs) {
      append_wrapped_block(lines, "  ", ref, content_width);
    }
  }

  lines.push_back("");
  lines.push_back("Touched files");
  if (detail.files.empty()) {
    lines.push_back("  (none)");
  } else {
    for (const auto &file : detail.files) {
      append_wrapped_block(lines, "  ", file, content_width);
    }
  }

  lines.push_back("");
  lines.push_back("Turns");
  if (detail.turns.empty()) {
    lines.push_back("  (none)");
  } else {
    for (const auto &turn : detail.turns) {
      lines.push_back("  Turn " + std::to_string(turn.turn_index) + "  " +
                      turn.timestamp);
      append_wrapped_block(lines, "    user> ", turn.user_message,
                           content_width);
      append_wrapped_block(lines, "    asst> ", turn.assistant_response,
                           content_width);
      lines.push_back("");
    }
  }

  lines.push_back("Checkpoints");
  if (detail.checkpoints.empty()) {
    lines.push_back("  (none)");
  } else {
    for (const auto &checkpoint : detail.checkpoints) {
      lines.push_back(
          "  #" + std::to_string(checkpoint.checkpoint_number) + " " +
          (checkpoint.title.empty() ? "(untitled)" : checkpoint.title));
      append_wrapped_block(lines, "    overview: ", checkpoint.overview,
                           content_width);
      append_wrapped_block(lines, "    work:     ", checkpoint.work_done,
                           content_width);
      append_wrapped_block(lines, "    next:     ", checkpoint.next_steps,
                           content_width);
      lines.push_back("");
    }
  }

  return lines;
}

std::vector<std::string> build_help_modal_lines(const AppState &state, int width) {
  std::vector<std::string> lines;
  const int content_width = std::max(12, width);

  append_wrapped_block(lines, "Version   ", project_version(), content_width);
  append_wrapped_block(lines, "Database  ", configured_db_path(state),
                       content_width);
  append_wrapped_block(lines, "Theme     ", theme_mode_label(state.theme_mode),
                       content_width);

  lines.push_back("");
  lines.push_back("Browser");
  lines.push_back("  Tab, h, l  switch focus between folders and sessions");
  lines.push_back("  j, k       move selection");
  lines.push_back("  u, d       page up/down");
  lines.push_back("  gg, ge     jump to top or bottom");
  lines.push_back("  su, sc     sort by last update or creation time (desc)");
  lines.push_back("  sua, sca   sort ascending");
  lines.push_back("  sud, scd   sort descending");
  lines.push_back("  :          open command mode");
  lines.push_back("  y          yank selected session ID");
  lines.push_back("  c          continue selected session");
  lines.push_back("  Space      toggle full folder path preview");
  lines.push_back("  Enter      open session detail modal");
  lines.push_back("  r          reload the current database");
  lines.push_back("  q          quit");

  lines.push_back("");
  lines.push_back("Command mode");
  lines.push_back("  :help              show this help modal");
  lines.push_back("  :theme dark        switch to dark theme");
  lines.push_back("  :theme light       switch to light theme");
  lines.push_back("  :open <db path>    open another session-store.db");

  lines.push_back("");
  lines.push_back("Modal");
  lines.push_back("  j, k       scroll");
  lines.push_back("  u, d       page up/down");
  lines.push_back("  gg         jump to top");
  lines.push_back("  ge         jump to bottom");
  lines.push_back("  Enter, Esc, q  close");

  lines.push_back("");
  lines.push_back("Notes");
  lines.push_back("  Startup database order:");
  lines.push_back("    1. $COPILOT_HOME/session-store.db");
  lines.push_back("    2. $HOME/.copilot/session-store.db");
  lines.push_back("  :open accepts absolute, relative, and ~/ paths.");
  lines.push_back("  Resume starts Copilot from the stored session cwd.");
  lines.push_back("  Resume is blocked if that folder no longer exists.");

  return lines;
}

const char *modal_title(const AppState &state) {
  return state.modal_kind == ModalKind::Help ? " Help " : " Session Detail Modal ";
}

std::string modal_subject(const AppState &state) {
  return state.modal_kind == ModalKind::Help ? "help" : "session detail";
}

std::vector<std::string> active_modal_lines(const AppState &state, int width) {
  if (state.modal_kind == ModalKind::Help) {
    return build_help_modal_lines(state, width);
  }
  return build_modal_lines(state.detail, width);
}

int modal_content_width(int cols) {
  const int modal_w = std::min(cols - 8, 96);
  return modal_w - 4;
}

int modal_max_scroll(const AppState &state, int cols, int modal_rows) {
  if (state.modal_kind == ModalKind::SessionDetail && !state.detail.loaded) {
    return 0;
  }
  const auto lines = active_modal_lines(state, modal_content_width(cols));
  return std::max(0, static_cast<int>(lines.size()) - modal_rows);
}

std::string current_filter_value(const AppState &state) {
  if (state.cwd_filters.empty() || state.selected_cwd_index < 0 ||
      state.selected_cwd_index >= static_cast<int>(state.cwd_filters.size())) {
    return "";
  }
  return state.cwd_filters[state.selected_cwd_index].value;
}

const CwdFilter *current_filter(const AppState &state) {
  if (state.cwd_filters.empty() || state.selected_cwd_index < 0 ||
      state.selected_cwd_index >= static_cast<int>(state.cwd_filters.size())) {
    return nullptr;
  }
  return &state.cwd_filters[state.selected_cwd_index];
}

void clamp_filter_selection(AppState &state) {
  if (state.cwd_filters.empty()) {
    state.selected_cwd_index = 0;
    state.cwd_scroll_offset = 0;
    return;
  }
  const int max_index = static_cast<int>(state.cwd_filters.size()) - 1;
  state.selected_cwd_index = std::clamp(state.selected_cwd_index, 0, max_index);
  state.cwd_scroll_offset = std::clamp(state.cwd_scroll_offset, 0, max_index);
}

void ensure_filter_visible(AppState &state, int visible_rows) {
  if (state.cwd_filters.empty() || visible_rows <= 0) {
    state.cwd_scroll_offset = 0;
    return;
  }

  const int max_offset =
      std::max(0, static_cast<int>(state.cwd_filters.size()) - visible_rows);
  if (state.selected_cwd_index < state.cwd_scroll_offset) {
    state.cwd_scroll_offset = state.selected_cwd_index;
  } else if (state.selected_cwd_index >=
             state.cwd_scroll_offset + visible_rows) {
    state.cwd_scroll_offset = state.selected_cwd_index - visible_rows + 1;
  }
  state.cwd_scroll_offset = std::clamp(state.cwd_scroll_offset, 0, max_offset);
}

void apply_current_filter(AppState &state) {
  const SessionEntry *selected = current_session(state);
  const std::string selected_id = selected == nullptr ? "" : selected->id;
  const std::string filter_value = current_filter_value(state);
  const bool show_all = state.cwd_filters.empty() ||
                        state.cwd_filters[state.selected_cwd_index].is_all;

  state.sessions.clear();
  for (const auto &session : state.all_sessions) {
    if (show_all || session.cwd == filter_value) {
      state.sessions.push_back(session);
    }
  }

  state.selected_index = 0;
  for (int i = 0; i < static_cast<int>(state.sessions.size()); ++i) {
    if (state.sessions[i].id == selected_id) {
      state.selected_index = i;
      break;
    }
  }
  state.scroll_offset = 0;
  state.detail = {};
  state.modal_open = false;
  state.modal_kind = ModalKind::SessionDetail;
  state.modal_scroll_offset = 0;
  state.modal_g_pending = false;

  const std::string label =
      state.cwd_filters.empty()
          ? "ALL"
          : state.cwd_filters[state.selected_cwd_index].label;
  state.status = "Showing " + std::to_string(state.sessions.size()) +
                 " sessions for " + label;
}

void rebuild_cwd_filters(AppState &state, const std::string &preferred_value) {
  std::vector<std::string> unique_cwds;
  for (const auto &session : state.all_sessions) {
    std::string cwd = session.cwd.empty() ? "(none)" : session.cwd;
    unique_cwds.push_back(cwd);
  }

  std::sort(unique_cwds.begin(), unique_cwds.end());
  unique_cwds.erase(std::unique(unique_cwds.begin(), unique_cwds.end()),
                    unique_cwds.end());

  state.cwd_filters.clear();
  state.cwd_filters.push_back({"ALL", "", true, true});
  for (const auto &cwd : unique_cwds) {
    const std::string value = cwd == "(none)" ? "" : cwd;
    state.cwd_filters.push_back({cwd, value, false, directory_exists(value)});
  }

  state.selected_cwd_index = 0;
  for (int i = 0; i < static_cast<int>(state.cwd_filters.size()); ++i) {
    const auto &filter = state.cwd_filters[i];
    if ((!filter.is_all && filter.value == preferred_value) ||
        (filter.is_all && preferred_value.empty())) {
      state.selected_cwd_index = i;
      break;
    }
  }
  state.cwd_scroll_offset = 0;
}

bool load_sessions(AppState &state) {
  const std::string preferred_filter = current_filter_value(state);
  if (state.db_path.empty()) {
    state.db_path = default_db_path();
    if (state.db_path.empty()) {
      state.all_sessions.clear();
      state.sessions.clear();
      state.cwd_filters.clear();
      state.selected_index = 0;
      state.scroll_offset = 0;
      state.selected_cwd_index = 0;
      state.cwd_scroll_offset = 0;
      state.status =
          "COPILOT_HOME and HOME are not set; cannot locate session-store.db";
      state.db_path.clear();
      return false;
    }
  } else {
    state.db_path = normalize_path(state.db_path);
  }

  if (state.db_path.empty()) {
    state.all_sessions.clear();
    state.sessions.clear();
    state.cwd_filters.clear();
    state.selected_index = 0;
    state.scroll_offset = 0;
    state.selected_cwd_index = 0;
    state.cwd_scroll_offset = 0;
    state.status = "Database path is empty.";
    return false;
  }
  std::error_code db_error;
  const auto db_size = fs::file_size(state.db_path, db_error);
  state.db_size_label =
      db_error ? "(unknown)" : format_bytes(static_cast<long long>(db_size));
  sqlite3 *db = nullptr;
  const int open_rc = sqlite3_open_v2(state.db_path.c_str(), &db,
                                      SQLITE_OPEN_READONLY, nullptr);
  if (open_rc != SQLITE_OK) {
    state.all_sessions.clear();
    state.sessions.clear();
    state.cwd_filters.clear();
    state.selected_index = 0;
    state.scroll_offset = 0;
    state.selected_cwd_index = 0;
    state.cwd_scroll_offset = 0;
    const char *message = db != nullptr ? sqlite3_errmsg(db) : "open failed";
    state.status = "Failed to open " + state.db_path + ": " +
                   std::string(message);
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  const char *query =
      "SELECT id, COALESCE(summary, ''), COALESCE(created_at, ''), "
      "COALESCE(updated_at, ''), COALESCE(cwd, ''), "
      "COALESCE(repository, '') "
      "FROM sessions;";

  sqlite3_stmt *stmt = nullptr;
  const int prepare_rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (prepare_rc != SQLITE_OK) {
    state.all_sessions.clear();
    state.sessions.clear();
    state.cwd_filters.clear();
    state.selected_index = 0;
    state.scroll_offset = 0;
    state.selected_cwd_index = 0;
    state.cwd_scroll_offset = 0;
    state.status =
        "Failed to query sessions: " + std::string(sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  std::vector<SessionEntry> sessions;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    SessionEntry session;
    session.id = column_text(stmt, 0);
    session.summary = column_text(stmt, 1);
    session.created_at = column_text(stmt, 2);
    session.updated_at = column_text(stmt, 3);
    session.cwd = column_text(stmt, 4);
    session.repository = column_text(stmt, 5);
    sessions.push_back(std::move(session));
  }

  const int finalize_rc = sqlite3_finalize(stmt);
  if (finalize_rc != SQLITE_OK) {
    state.all_sessions.clear();
    state.sessions.clear();
    state.cwd_filters.clear();
    state.selected_index = 0;
    state.scroll_offset = 0;
    state.selected_cwd_index = 0;
    state.cwd_scroll_offset = 0;
    state.status =
        "Failed to finalize session query: " + std::string(sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  sort_sessions(sessions, state.sort_mode);
  state.all_sessions = std::move(sessions);
  rebuild_cwd_filters(state, preferred_filter);
  apply_current_filter(state);

  if (state.all_sessions.empty()) {
    state.selected_index = 0;
    state.scroll_offset = 0;
    state.status = "No stored sessions found in " + state.db_path;
    return true;
  }

  state.status = "Loaded " + std::to_string(state.all_sessions.size()) +
                 " Copilot sessions from " + state.db_path;
  return true;
}

bool load_session_detail(AppState &state, const SessionEntry &entry) {
  state.detail = {};
  state.detail.id = entry.id;
  state.detail.summary = entry.summary;
  state.detail.created_at = entry.created_at;
  state.detail.cwd = entry.cwd;
  state.detail.repository = entry.repository;
  state.modal_scroll_offset = 0;
  state.modal_g_pending = false;

  sqlite3 *db = nullptr;
  const int open_rc = sqlite3_open_v2(state.db_path.c_str(), &db,
                                      SQLITE_OPEN_READONLY, nullptr);
  if (open_rc != SQLITE_OK) {
    state.status = "Failed to open " + configured_db_path(state) +
                   " for detail view.";
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  const char *summary_query =
      "SELECT "
      "COALESCE(updated_at, ''), "
      "COALESCE(branch, ''), "
      "COALESCE(host_type, ''), "
      "(SELECT COUNT(*) FROM turns WHERE session_id = ?1), "
      "(SELECT COUNT(*) FROM checkpoints WHERE session_id = ?1), "
      "(SELECT COUNT(*) FROM session_files WHERE session_id = ?1), "
      "(SELECT COUNT(*) FROM session_refs WHERE session_id = ?1) "
      "FROM sessions WHERE id = ?1;";

  sqlite3_stmt *stmt = nullptr;
  const int prepare_rc =
      sqlite3_prepare_v2(db, summary_query, -1, &stmt, nullptr);
  if (prepare_rc != SQLITE_OK) {
    state.status = "Failed to prepare session detail query.";
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);

  const int step_rc = sqlite3_step(stmt);
  if (step_rc == SQLITE_ROW) {
    state.detail.updated_at = column_text(stmt, 0);
    state.detail.branch = column_text(stmt, 1);
    state.detail.host_type = column_text(stmt, 2);
    state.detail.turn_count = sqlite3_column_int(stmt, 3);
    state.detail.checkpoint_count = sqlite3_column_int(stmt, 4);
    state.detail.file_count = sqlite3_column_int(stmt, 5);
    state.detail.ref_count = sqlite3_column_int(stmt, 6);
    state.detail.loaded = true;
  } else {
    state.status = "Failed to load session details.";
  }

  sqlite3_finalize(stmt);
  if (!state.detail.loaded) {
    sqlite3_close(db);
    return false;
  }

  const char *turns_query =
      "SELECT turn_index, COALESCE(user_message, ''), "
      "COALESCE(assistant_response, ''), COALESCE(timestamp, '') "
      "FROM turns WHERE session_id = ?1 ORDER BY turn_index ASC;";
  if (sqlite3_prepare_v2(db, turns_query, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      SessionTurn turn;
      turn.turn_index = sqlite3_column_int(stmt, 0);
      turn.user_message = column_text(stmt, 1);
      turn.assistant_response = column_text(stmt, 2);
      turn.timestamp = column_text(stmt, 3);
      state.detail.turns.push_back(std::move(turn));
    }
    sqlite3_finalize(stmt);
  }

  const char *checkpoints_query =
      "SELECT checkpoint_number, COALESCE(title, ''), COALESCE(overview, ''), "
      "COALESCE(work_done, ''), COALESCE(next_steps, '') "
      "FROM checkpoints WHERE session_id = ?1 ORDER BY checkpoint_number ASC;";
  if (sqlite3_prepare_v2(db, checkpoints_query, -1, &stmt, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      SessionCheckpoint checkpoint;
      checkpoint.checkpoint_number = sqlite3_column_int(stmt, 0);
      checkpoint.title = column_text(stmt, 1);
      checkpoint.overview = column_text(stmt, 2);
      checkpoint.work_done = column_text(stmt, 3);
      checkpoint.next_steps = column_text(stmt, 4);
      state.detail.checkpoints.push_back(std::move(checkpoint));
    }
    sqlite3_finalize(stmt);
  }

  const char *refs_query =
      "SELECT COALESCE(ref_type, ''), COALESCE(ref_value, '') "
      "FROM session_refs WHERE session_id = ?1 ORDER BY created_at ASC;";
  if (sqlite3_prepare_v2(db, refs_query, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      state.detail.refs.push_back("[" + std::string(column_text(stmt, 0)) +
                                  "] " + std::string(column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
  }

  const char *files_query = "SELECT COALESCE(file_path, '') FROM session_files "
                            "WHERE session_id = ?1 ORDER BY file_path ASC;";
  if (sqlite3_prepare_v2(db, files_query, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      state.detail.files.push_back(column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
  }

  sqlite3_close(db);
  return state.detail.loaded;
}

void clamp_selection(AppState &state) {
  if (state.sessions.empty()) {
    state.selected_index = 0;
    state.scroll_offset = 0;
    return;
  }
  const int max_index = static_cast<int>(state.sessions.size()) - 1;
  state.selected_index = std::clamp(state.selected_index, 0, max_index);
  state.scroll_offset = std::clamp(state.scroll_offset, 0, max_index);
}

void ensure_visible(AppState &state, int visible_rows) {
  if (state.sessions.empty() || visible_rows <= 0) {
    state.scroll_offset = 0;
    return;
  }

  const int max_offset =
      std::max(0, static_cast<int>(state.sessions.size()) - visible_rows);
  if (state.selected_index < state.scroll_offset) {
    state.scroll_offset = state.selected_index;
  } else if (state.selected_index >= state.scroll_offset + visible_rows) {
    state.scroll_offset = state.selected_index - visible_rows + 1;
  }
  state.scroll_offset = std::clamp(state.scroll_offset, 0, max_offset);
}

void draw_session_list(ncplane *plane, int y, int x, int height, int width,
                       const AppState &state, const Theme &theme,
                       bool focused) {
  if (height <= 0 || width <= 0) {
    return;
  }

  if (state.sessions.empty()) {
    set_colors(plane, theme.dim, theme.panel_bg);
    ncplane_putstr_yx(plane, y, x, "No sessions available.");
    reset_colors(plane);
    return;
  }

  constexpr int id_width = 8;
  constexpr int min_summary_width = 8;
  int timestamp_width = 0;
  std::string meta_template;
  const int meta_budget =
      width - (2 + id_width + 2 + min_summary_width + 1);
  if (meta_budget >= 4) {
    timestamp_width = std::max(4, std::min(16, meta_budget));
    meta_template = std::string(timestamp_width, ' ');
  }
  const int summary_width =
      std::max(min_summary_width,
               width - (2 + id_width + 2 +
                        (meta_template.empty()
                             ? 0
                             : 1 + static_cast<int>(meta_template.size()))));

  for (int row = 0; row < height; ++row) {
    const int index = state.scroll_offset + row;
    if (index >= static_cast<int>(state.sessions.size())) {
      break;
    }

    const SessionEntry &session = state.sessions[index];
    const bool selected = index == state.selected_index;
    const Rgb &bg = selected ? (focused ? theme.accent : theme.panel_alt_bg)
                             : theme.panel_bg;
    const Rgb &fg =
        selected ? (focused ? theme.base_bg : theme.warm) : theme.chrome_fg;
    fill_rect(plane, y + row, x, 1, width, bg);
    set_colors(plane, fg, bg, selected ? NCSTYLE_BOLD : NCSTYLE_NONE);

    const std::string summary =
        session.summary.empty() ? "(no summary)" : session.summary;
    std::string line = selected ? "> " : "  ";
    line += shorten_id(session.id);
    line += "  ";
    line += ellipsize(summary, summary_width);
    if (timestamp_width > 0) {
      const std::string date_meta = format_list_timestamp(
          sort_primary_timestamp(session, state.sort_mode), timestamp_width);
      const int used = static_cast<int>(line.size());
      if (used < width - static_cast<int>(date_meta.size())) {
        line.append(width - static_cast<int>(date_meta.size()) - used, ' ');
      }
      line += date_meta;
    }

    ncplane_printf_yx(plane, y + row, x, "%.*s", width, line.c_str());
    reset_colors(plane);
  }
}

void draw_cwd_list(ncplane *plane, int y, int x, int height, int width,
                   const AppState &state, const Theme &theme, bool focused) {
  if (height <= 0 || width <= 0) {
    return;
  }

  if (state.cwd_filters.empty()) {
    set_colors(plane, theme.dim, theme.panel_bg);
    ncplane_putstr_yx(plane, y, x, "No CWD filters.");
    reset_colors(plane);
    return;
  }

  for (int row = 0; row < height; ++row) {
    const int index = state.cwd_scroll_offset + row;
    if (index >= static_cast<int>(state.cwd_filters.size())) {
      break;
    }

    const auto &filter = state.cwd_filters[index];
    const bool selected = index == state.selected_cwd_index;
    const bool missing = !filter.is_all && !filter.exists;
    const Rgb &bg = selected ? (focused ? theme.accent : theme.panel_bg)
                             : theme.panel_alt_bg;
    const Rgb &fg = missing
                        ? theme.alert
                        : (selected ? (focused ? theme.base_bg : theme.accent_2)
                                    : theme.chrome_fg);
    fill_rect(plane, y + row, x, 1, width, bg);
    set_colors(plane, fg, bg, selected ? NCSTYLE_BOLD : NCSTYLE_NONE);

    std::string line =
        selected ? (state.show_selected_cwd_path ? "* " : "> ") : "  ";
    line += missing ? "! " : "  ";
    line += ellipsize(filter.label, width - 4);
    ncplane_printf_yx(plane, y + row, x, "%.*s", width, line.c_str());
    reset_colors(plane);
  }
}

void draw_modal(const AppState &state, ncplane *plane, const Theme &theme,
                int rows, int cols) {
  const int modal_h = std::min(rows - 4, 18);
  const int modal_w = std::min(cols - 8, 96);
  const int modal_y = (rows - modal_h) / 2;
  const int modal_x = (cols - modal_w) / 2;

  fill_rect(plane, modal_y - 1, modal_x - 2, modal_h + 2, modal_w + 4,
            theme.base_bg);
  draw_box(plane, modal_y, modal_x, modal_h, modal_w, modal_title(state),
           theme, theme.panel_alt_bg, theme.accent_2, true);

  if (state.modal_kind == ModalKind::SessionDetail && !state.detail.loaded) {
    set_colors(plane, theme.alert, theme.panel_alt_bg, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, modal_y + 2, modal_x + 2,
                      "Session detail could not be loaded.");
    reset_colors(plane);
    return;
  }

  const int content_y = modal_y + 2;
  const int content_h = modal_h - 4;
  const auto lines = active_modal_lines(state, modal_w - 4);
  const int max_scroll =
      std::max(0, static_cast<int>(lines.size()) - content_h);
  const int scroll = std::clamp(state.modal_scroll_offset, 0, max_scroll);

  for (int row = 0; row < content_h; ++row) {
    const int line_index = scroll + row;
    if (line_index >= static_cast<int>(lines.size())) {
      break;
    }

    const std::string &line = lines[line_index];
    const bool section_header =
        line == "Refs" || line == "Touched files" || line == "Turns" ||
        line == "Checkpoints" || line == "Browser" || line == "Command mode" ||
        line == "Modal" || line == "Notes";
    const bool user_line = line.rfind("    user> ", 0) == 0;
    const bool assistant_line = line.rfind("    asst> ", 0) == 0;

    if (section_header) {
      set_colors(plane, theme.warm, theme.panel_alt_bg, NCSTYLE_BOLD);
    } else if (user_line) {
      set_colors(plane, theme.user, theme.panel_alt_bg, NCSTYLE_BOLD);
    } else if (assistant_line) {
      set_colors(plane, theme.accent_2, theme.panel_alt_bg, NCSTYLE_BOLD);
    } else {
      set_colors(plane, theme.chrome_fg, theme.panel_alt_bg);
    }
    ncplane_printf_yx(plane, content_y + row, modal_x + 2, "%.*s", modal_w - 4,
                      line.c_str());
  }

  set_colors(plane, theme.dim, theme.panel_alt_bg);
  ncplane_printf_yx(
      plane, modal_y + modal_h - 2, modal_x + 2, "%.*s", modal_w - 4,
      ("j/k scroll  u/d page  gg top  ge bottom  c continue session  Enter/Esc "
       "close "
       " " +
       std::to_string(scroll + 1) + "/" + std::to_string(max_scroll + 1))
          .c_str());
  reset_colors(plane);
}

void draw_resume_warning_modal(const AppState &state, ncplane *plane,
                               const Theme &theme, int rows, int cols) {
  const int modal_h = 11;
  const int modal_w = std::min(cols - 8, 92);
  const int modal_y = (rows - modal_h) / 2;
  const int modal_x = (cols - modal_w) / 2;

  fill_rect(plane, modal_y - 1, modal_x - 2, modal_h + 2, modal_w + 4,
            theme.base_bg);
  draw_box(plane, modal_y, modal_x, modal_h, modal_w,
           " Missing session folder ", theme, theme.panel_alt_bg, theme.alert,
           true);

  set_colors(plane, theme.alert, theme.panel_alt_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(
      plane, modal_y + 2, modal_x + 2, "%.*s", modal_w - 4,
      "The original folder for this session does not exist anymore.");

  set_colors(plane, theme.chrome_fg, theme.panel_alt_bg);
  const auto cwd_lines =
      wrap_text("Missing folder: " + (state.pending_resume_session_cwd.empty()
                                          ? std::string("(none)")
                                          : state.pending_resume_session_cwd),
                modal_w - 4);
  for (int row = 0; row < 3 && row < static_cast<int>(cwd_lines.size());
       ++row) {
    ncplane_printf_yx(plane, modal_y + 4 + row, modal_x + 2, "%.*s",
                      modal_w - 4, cwd_lines[row].c_str());
  }

  ncplane_printf_yx(
      plane, modal_y + 8, modal_x + 2, "%.*s", modal_w - 4,
      "Resume is blocked until that folder exists again. Press Enter/Esc/q.");
  reset_colors(plane);
}

void draw_clipboard_modal(const AppState &state, ncplane *plane,
                          const Theme &theme, int rows, int cols) {
  const int modal_h = 11;
  const int modal_w = std::min(cols - 8, 92);
  const int modal_y = (rows - modal_h) / 2;
  const int modal_x = (cols - modal_w) / 2;

  fill_rect(plane, modal_y - 1, modal_x - 2, modal_h + 2, modal_w + 4,
            theme.base_bg);
  draw_box(plane, modal_y, modal_x, modal_h, modal_w, " Session ID yanked ",
           theme, theme.panel_alt_bg, theme.good, true);

  set_colors(plane, theme.good, theme.panel_alt_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, modal_y + 2, modal_x + 2, "%.*s", modal_w - 4,
                    "The following session ID was copied to the clipboard:");

  set_colors(plane, theme.chrome_fg, theme.panel_alt_bg);
  const auto lines = wrap_text(state.clipboard_session_id, modal_w - 4);
  for (int row = 0; row < 3 && row < static_cast<int>(lines.size()); ++row) {
    ncplane_printf_yx(plane, modal_y + 4 + row, modal_x + 2, "%.*s",
                      modal_w - 4, lines[row].c_str());
  }

  const std::string ok_label = " OK ";
  const int ok_x = modal_x + (modal_w - static_cast<int>(ok_label.size())) / 2;
  set_colors(plane, theme.base_bg, theme.good, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, modal_y + 8, ok_x, "%s", ok_label.c_str());
  reset_colors(plane);
}

void render(const AppState &state, ncplane *plane) {
  const Theme theme = current_theme(state);
  unsigned rows_u = 0;
  unsigned cols_u = 0;
  ncplane_dim_yx(plane, &rows_u, &cols_u);
  const int rows = static_cast<int>(rows_u);
  const int cols = static_cast<int>(cols_u);
  ncplane_erase(plane);
  fill_rect(plane, 0, 0, rows, cols, theme.base_bg);

  if (rows < 18 || cols < 72) {
    set_colors(plane, theme.warm, theme.base_bg, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, 1, 2,
                      "Terminal too small for the session browser.");
    set_colors(plane, theme.chrome_fg, theme.base_bg);
    ncplane_putstr_yx(plane, 3, 2,
                      "Resize to at least 72x18 or press q to quit.");
    reset_colors(plane);
    return;
  }

  const int header_h = 4;
  const int footer_h = 3;
  const int detail_h = 8;
  const int body_y = header_h;
  const int body_h = rows - header_h - detail_h - footer_h;
  const int filter_w = std::max(26, std::min(34, cols / 4));
  const int list_x = filter_w;
  const int list_w = cols - filter_w;
  const int detail_y = body_y + body_h;
  const int footer_y = detail_y + detail_h;
  const int visible_rows = session_visible_rows(rows);
  const int cwd_rows = cwd_visible_rows(rows);

  draw_box(plane, 0, 0, header_h, cols, " Copilot Sessions ", theme,
           theme.panel_bg, theme.accent, true);
  draw_box(plane, body_y, 0, body_h, filter_w, " CWD Filters ", theme,
           theme.panel_alt_bg, theme.accent, state.focus == FocusPane::Cwds);
  draw_box(plane, body_y, list_x, body_h, list_w, " Stored Sessions ", theme,
           theme.panel_bg, theme.accent_2, true);
  draw_box(plane, detail_y, 0, detail_h, cols, " Selected Session ", theme,
           theme.panel_alt_bg, theme.user);
  draw_box(plane, footer_y, 0, footer_h, cols, " Status Bus ", theme,
           theme.panel_bg, theme.good);

  set_colors(plane, theme.chrome_fg, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, 1, 2, "%.*s", std::max(0, cols - 23),
                    ellipsize("copilot-sessions v" + project_version() +
                                  " | Session browser for " +
                                  configured_db_path(state),
                              std::max(0, cols - 23))
                        .c_str());
  set_colors(plane, theme.accent_2, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, 1, cols - 19, "[ SQLITE LIVE ]");
  set_colors(plane, theme.dim, theme.panel_bg);
  ncplane_putstr_yx(
      plane, 2, 2,
      "Use Tab or h/l to change pane, j/k browse, : command, y yank ID, "
      "c continue, su/sc sort, Enter modal");
  reset_colors(plane);

  set_colors(plane, theme.chrome_fg, theme.panel_alt_bg, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, body_y + 2, 2, "Folders");
  set_colors(plane, theme.accent_2, theme.panel_alt_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, body_y + 2, filter_w - 6, "%3zu",
                    state.cwd_filters.size());
  reset_colors(plane);

  set_colors(plane, theme.chrome_fg, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, body_y + 2, list_x + 2, "Sessions");
  set_colors(plane, theme.accent_2, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, body_y + 2, list_x + list_w - 6, "%3zu",
                    state.sessions.size());
  set_colors(plane, theme.chrome_fg, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, body_y + 3, list_x + 2, "Selection");
  set_colors(plane, theme.user, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, body_y + 3, list_x + 14, "%2d/%-2zu",
                    state.sessions.empty() ? 0 : state.selected_index + 1,
                    state.sessions.size());
  set_colors(plane, theme.chrome_fg, theme.panel_bg, NCSTYLE_BOLD);
  ncplane_printf_yx(plane, body_y + 3, list_x + std::max(18, list_w - 28),
                    "%.*s", std::max(0, list_w - 20),
                    ellipsize("Sort: " +
                                  std::string(session_sort_key_label(state.sort_mode)) +
                                  " (" +
                                  std::string(session_sort_label(state.sort_mode)) +
                                  ")",
                              std::max(0, list_w - 20))
                        .c_str());
  reset_colors(plane);
  draw_cwd_list(plane, body_y + 3, 2, cwd_rows, filter_w - 4, state, theme,
                state.focus == FocusPane::Cwds);
  draw_session_list(plane, body_y + 5, list_x + 2, visible_rows, list_w - 4,
                    state, theme, state.focus == FocusPane::Sessions);

  const SessionEntry *selected = current_session(state);

  const CwdFilter *filter = current_filter(state);
  const bool show_cwd_preview = state.show_selected_cwd_path &&
                                state.focus == FocusPane::Cwds &&
                                filter != nullptr && !filter->is_all;

  if (show_cwd_preview) {
    set_colors(plane, theme.accent_2, theme.panel_alt_bg, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, detail_y + 1, 2, "FILTER PATH");

    const auto wrapped = wrap_text(filter->value, std::max(1, cols - 4));
    set_colors(plane, theme.chrome_fg, theme.panel_alt_bg);
    for (int row = 0; row < 6; ++row) {
      if (row >= static_cast<int>(wrapped.size())) {
        break;
      }
      ncplane_printf_yx(plane, detail_y + 2 + row, 2, "%.*s", cols - 4,
                        wrapped[row].c_str());
    }
    reset_colors(plane);
  } else if (selected == nullptr) {
    set_colors(plane, theme.dim, theme.panel_alt_bg);
    ncplane_putstr_yx(plane, detail_y + 2, 2, "No session selected.");
    reset_colors(plane);
  } else {
    set_colors(plane, theme.user, theme.panel_alt_bg, NCSTYLE_BOLD);
    ncplane_printf_yx(plane, detail_y + 1, 2, "ID      %.*s", cols - 10,
                      selected->id.c_str());
    set_colors(plane, theme.chrome_fg, theme.panel_alt_bg);
    ncplane_printf_yx(
        plane, detail_y + 2, 2, "SUMMARY %.*s", cols - 10,
        (selected->summary.empty() ? "(no summary)" : selected->summary)
            .c_str());
    ncplane_printf_yx(plane, detail_y + 3, 2, "CREATED %.*s", cols - 10,
                      selected->created_at.c_str());
    ncplane_printf_yx(
        plane, detail_y + 4, 2, "UPDATED %.*s", cols - 10,
        selected->updated_at.c_str());
    ncplane_printf_yx(
        plane, detail_y + 5, 2, "REPO    %.*s", cols - 10,
        ellipsize((selected->repository.empty() ? "(none)" : selected->repository) +
                      std::string(" | FILTER ") +
                      (filter == nullptr ? "ALL" : filter->label),
                  cols - 10)
            .c_str());
    ncplane_printf_yx(
        plane, detail_y + 6, 2, "CWD     %.*s", cols - 10,
        (selected->cwd.empty() ? "(none)" : selected->cwd).c_str());
    set_colors(plane, theme.dim, theme.panel_alt_bg);
    ncplane_printf_yx(
        plane, detail_y + 7, 2, "%.*s", cols - 4,
        ellipsize("Scope: " +
                      std::string(state.focus == FocusPane::Cwds
                                      ? "CWD filters"
                                      : "session list") +
                      " | Database: " +
                      configured_db_path(state) +
                      " (" + state.db_size_label + ")" +
                      " | Theme: " + theme_mode_label(state.theme_mode) +
                      " | Hotkeys: Tab/h/l switch pane, j/k browse, "
                      "su/sc + a/d sort, : command, y yank ID, c continue, "
                      "Space full path, Enter modal, r reload, q quit",
                  cols - 4)
            .c_str());
    reset_colors(plane);
  }

  if (state.command_mode) {
    set_colors(plane, theme.accent_2, theme.panel_bg, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, footer_y + 1, 2, "CMD");
    set_colors(plane, theme.chrome_fg, theme.panel_bg);
    ncplane_printf_yx(plane, footer_y + 1, 7, "%.*s", cols - 9,
                      state.command_buffer.c_str());
  } else {
    set_colors(plane, theme.good, theme.panel_bg, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, footer_y + 1, 2, "SYNC");
    set_colors(plane, theme.chrome_fg, theme.panel_bg);
    ncplane_printf_yx(plane, footer_y + 1, 8, "%.*s", cols - 10,
                      state.status.c_str());
  }
  reset_colors(plane);

  if (state.modal_open) {
    draw_modal(state, plane, theme, rows, cols);
  }
  if (state.resume_warning_open) {
    draw_resume_warning_modal(state, plane, theme, rows, cols);
  }
  if (state.clipboard_modal_open) {
    draw_clipboard_modal(state, plane, theme, rows, cols);
  }
}

void move_selection(AppState &state, int delta, int visible_rows) {
  if (state.sessions.empty()) {
    return;
  }
  state.selected_index += delta;
  clamp_selection(state);
  ensure_visible(state, visible_rows);
  state.status = "Selected session " +
                 std::to_string(state.selected_index + 1) + " of " +
                 std::to_string(state.sessions.size());
}

void move_filter_selection(AppState &state, int delta, int visible_rows) {
  if (state.cwd_filters.empty()) {
    return;
  }
  state.selected_cwd_index += delta;
  clamp_filter_selection(state);
  ensure_filter_visible(state, visible_rows);
  apply_current_filter(state);
}

void jump_browser_to_top(AppState &state) {
  if (state.focus == FocusPane::Cwds) {
    if (state.cwd_filters.empty()) {
      state.status = "No CWD filter available.";
      return;
    }
    state.selected_cwd_index = 0;
    state.cwd_scroll_offset = 0;
    apply_current_filter(state);
    state.status = "Jumped to top of CWD filters.";
    return;
  }

  if (state.sessions.empty()) {
    state.status = "No session available.";
    return;
  }
  state.selected_index = 0;
  state.scroll_offset = 0;
  state.status = "Jumped to top of session list.";
}

void jump_browser_to_bottom(AppState &state, int visible_rows, int filter_rows) {
  if (state.focus == FocusPane::Cwds) {
    if (state.cwd_filters.empty()) {
      state.status = "No CWD filter available.";
      return;
    }
    state.selected_cwd_index = static_cast<int>(state.cwd_filters.size()) - 1;
    ensure_filter_visible(state, filter_rows);
    apply_current_filter(state);
    state.status = "Jumped to bottom of CWD filters.";
    return;
  }

  if (state.sessions.empty()) {
    state.status = "No session available.";
    return;
  }
  state.selected_index = static_cast<int>(state.sessions.size()) - 1;
  ensure_visible(state, visible_rows);
  state.status = "Jumped to bottom of session list.";
}

void set_sort_mode(AppState &state, SessionSort mode, int visible_rows) {
  state.browser_g_pending = false;
  state.sort_s_pending = false;
  state.sort_direction_pending = false;
  if (state.sort_mode == mode) {
    state.status =
        "Already sorting sessions by " + std::string(session_sort_label(mode)) +
        ".";
    return;
  }

  state.sort_mode = mode;
  sort_sessions(state.all_sessions, state.sort_mode);
  apply_current_filter(state);
  ensure_visible(state, visible_rows);
  state.status = "Sorting sessions by " +
                 std::string(session_sort_label(state.sort_mode)) + ".";
}

SessionSort session_sort_with_direction(SessionSort mode, bool descending) {
  switch (mode) {
  case SessionSort::UpdatedDesc:
  case SessionSort::UpdatedAsc:
    return descending ? SessionSort::UpdatedDesc : SessionSort::UpdatedAsc;
  case SessionSort::CreatedDesc:
  case SessionSort::CreatedAsc:
    return descending ? SessionSort::CreatedDesc : SessionSort::CreatedAsc;
  }
  return descending ? SessionSort::UpdatedDesc : SessionSort::UpdatedAsc;
}

bool handle_key(uint32_t key, AppState &state, int visible_rows,
                int filter_rows, int modal_rows, int cols) {
  if (state.clipboard_modal_open) {
    switch (key) {
    case NCKEY_ENTER:
    case '\n':
    case '\r':
    case 27:
    case 'q':
      close_clipboard_modal(state, "Clipboard copy confirmed.");
      return true;
    default:
      return true;
    }
  }

  if (state.resume_warning_open) {
    switch (key) {
    case NCKEY_ENTER:
    case '\n':
    case '\r': {
      close_resume_warning(state,
                           "Resume blocked: stored session folder is missing.");
      return true;
    }
    case 27:
    case 'q':
      close_resume_warning(state, "Continue canceled.");
      return true;
    default:
      return true;
    }
  }

  if (state.modal_open) {
    if (state.modal_g_pending) {
      if (key == 'g') {
        state.modal_scroll_offset = 0;
        state.modal_g_pending = false;
        state.status = "Jumped to top of " + modal_subject(state) + " modal.";
        return true;
      }
      if (key == 'e') {
        state.modal_scroll_offset = modal_max_scroll(state, cols, modal_rows);
        state.modal_g_pending = false;
        state.status =
            "Jumped to bottom of " + modal_subject(state) + " modal.";
        return true;
      }
      state.modal_g_pending = false;
    }

    switch (key) {
    case 'g':
      state.modal_g_pending = true;
      state.status = "Modal chord started: g";
      return true;
    case 'j':
    case NCKEY_DOWN:
      state.modal_scroll_offset += 1;
      state.status = "Scrolled " + modal_subject(state) + " modal.";
      return true;
    case 'k':
    case NCKEY_UP:
      state.modal_scroll_offset = std::max(0, state.modal_scroll_offset - 1);
      state.status = "Scrolled " + modal_subject(state) + " modal.";
      return true;
    case 'd':
    case NCKEY_PGDOWN:
      state.modal_scroll_offset += modal_rows;
      state.status = "Scrolled " + modal_subject(state) + " modal.";
      return true;
    case 'u':
    case NCKEY_PGUP:
      state.modal_scroll_offset =
          std::max(0, state.modal_scroll_offset - modal_rows);
      state.status = "Scrolled " + modal_subject(state) + " modal.";
      return true;
    case 'c':
      if (state.modal_kind == ModalKind::SessionDetail) {
        return !queue_resume_selected_session(state);
      }
      return true;
    case NCKEY_ENTER:
    case '\n':
    case '\r':
    case 27:
    case 'q':
      state.modal_open = false;
      state.modal_g_pending = false;
      state.status = "Closed " + modal_subject(state) + " modal.";
      return true;
    default:
      return true;
    }
  }

  if (state.command_mode) {
    return handle_command_key(key, state);
  }

  if (state.browser_g_pending) {
    state.browser_g_pending = false;
    if (key == 'g') {
      jump_browser_to_top(state);
      return true;
    }
    if (key == 'e') {
      jump_browser_to_bottom(state, visible_rows, filter_rows);
      return true;
    }
  }

  if (state.sort_direction_pending) {
    state.sort_direction_pending = false;
    if (key == 'a') {
      set_sort_mode(state,
                    session_sort_with_direction(state.pending_sort_mode, false),
                    visible_rows);
      return true;
    }
    if (key == 'd') {
      set_sort_mode(state,
                    session_sort_with_direction(state.pending_sort_mode, true),
                    visible_rows);
      return true;
    }
  }

  if (state.sort_s_pending) {
    state.sort_s_pending = false;
    if (key == 'u') {
      state.pending_sort_mode = SessionSort::UpdatedDesc;
      set_sort_mode(state, SessionSort::UpdatedDesc, visible_rows);
      state.sort_direction_pending = true;
      state.status += " Press a or d to refine direction.";
      return true;
    }
    if (key == 'c') {
      state.pending_sort_mode = SessionSort::CreatedDesc;
      set_sort_mode(state, SessionSort::CreatedDesc, visible_rows);
      state.sort_direction_pending = true;
      state.status += " Press a or d to refine direction.";
      return true;
    }
    state.status = "Sort chord canceled.";
    return true;
  }

  switch (key) {
  case 'q':
    return false;
  case 'g':
    state.browser_g_pending = true;
    state.sort_s_pending = false;
    state.sort_direction_pending = false;
    state.status = "Browser chord started: g";
    return true;
  case 's':
    state.browser_g_pending = false;
    state.sort_s_pending = true;
    state.sort_direction_pending = false;
    state.status = "Sort chord started: use su or sc, then optional a/d.";
    return true;
  case ':':
    open_command_mode(state);
    return true;
  case '\t':
    focus_pane(state, state.focus == FocusPane::Cwds ? FocusPane::Sessions
                                                     : FocusPane::Cwds);
    return true;
  case 'h':
  case NCKEY_LEFT:
    focus_pane(state, FocusPane::Cwds);
    return true;
  case 'l':
  case NCKEY_RIGHT:
    focus_pane(state, FocusPane::Sessions);
    return true;
  case 'r':
    load_sessions(state);
    ensure_visible(state, visible_rows);
    ensure_filter_visible(state, filter_rows);
    return true;
  case 'c':
    return !queue_resume_selected_session(state);
  case 'y':
    yank_selected_session_id(state);
    return true;
  case 'j':
  case NCKEY_DOWN:
    if (state.focus == FocusPane::Cwds) {
      move_filter_selection(state, 1, filter_rows);
    } else {
      move_selection(state, 1, visible_rows);
    }
    return true;
  case 'k':
  case NCKEY_UP:
    if (state.focus == FocusPane::Cwds) {
      move_filter_selection(state, -1, filter_rows);
    } else {
      move_selection(state, -1, visible_rows);
    }
    return true;
  case 'd':
    if (state.focus == FocusPane::Cwds) {
      move_filter_selection(state, filter_rows, filter_rows);
    } else {
      move_selection(state, visible_rows, visible_rows);
    }
    return true;
  case 'u':
    if (state.focus == FocusPane::Cwds) {
      move_filter_selection(state, -filter_rows, filter_rows);
    } else {
      move_selection(state, -visible_rows, visible_rows);
    }
    return true;
  case NCKEY_ENTER:
  case '\n':
  case '\r':
    if (state.focus == FocusPane::Cwds) {
      state.focus = FocusPane::Sessions;
      state.status = "Filter applied. Focused session list.";
      return true;
    }
    if (state.sessions.empty()) {
      state.status = "No session selected.";
      return true;
    }
    if (load_session_detail(state, state.sessions[state.selected_index])) {
      state.modal_open = true;
      state.modal_kind = ModalKind::SessionDetail;
      state.status = "Opened detail modal for " +
                     shorten_id(state.sessions[state.selected_index].id);
    }
    return true;
  case ' ':
    if (state.focus != FocusPane::Cwds) {
      return true;
    }
    if (const CwdFilter *filter = current_filter(state);
        filter != nullptr && !filter->is_all) {
      state.show_selected_cwd_path = !state.show_selected_cwd_path;
      state.status = state.show_selected_cwd_path
                         ? "Showing full folder path for " + filter->value
                         : "Hid full folder path preview.";
    } else {
      state.show_selected_cwd_path = false;
      state.status = "Select a folder path to preview.";
    }
    return true;
  case NCKEY_PGDOWN:
    if (state.focus == FocusPane::Cwds) {
      move_filter_selection(state, filter_rows, filter_rows);
    } else {
      move_selection(state, visible_rows, visible_rows);
    }
    return true;
  case NCKEY_PGUP:
    if (state.focus == FocusPane::Cwds) {
      move_filter_selection(state, -filter_rows, filter_rows);
    } else {
      move_selection(state, -visible_rows, visible_rows);
    }
    return true;
  case NCKEY_RESIZE:
    ensure_visible(state, visible_rows);
    ensure_filter_visible(state, filter_rows);
    state.status = "Layout refreshed after resize.";
    return true;
  default:
    return true;
  }
}

} // namespace

int main() {
  AppState state;
  load_sessions(state);

  notcurses_options options = {};
  notcurses *nc = notcurses_init(&options, nullptr);
  if (nc == nullptr) {
    std::fprintf(stderr, "failed to initialize notcurses\n");
    return 1;
  }

  ncplane *stdplane = notcurses_stdplane(nc);
  bool running = true;

  while (running) {
    unsigned rows_u = 0;
    unsigned cols_u = 0;
    ncplane_dim_yx(stdplane, &rows_u, &cols_u);
    const int rows = static_cast<int>(rows_u);
    const int visible_rows = session_visible_rows(rows);
    const int filter_rows = cwd_visible_rows(rows);
    const int detail_rows = modal_visible_rows(rows);
    ensure_visible(state, visible_rows);
    ensure_filter_visible(state, filter_rows);

    render(state, stdplane);
    notcurses_render(nc);

    ncinput input = {};
    const uint32_t key = notcurses_get_blocking(nc, &input);
    running = handle_key(key, state, visible_rows, filter_rows, detail_rows,
                         static_cast<int>(cols_u));
  }

  notcurses_stop(nc);
  return resume_in_copilot(state);
}
