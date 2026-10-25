#include "core/filebrowser.h"

#include <SD.h>

#include <algorithm>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "core/storage.h"
#include "core/ui.h"

static const char* TAG = "browser";

// Protects RAM on cards with huge folders.
static const size_t kMaxEntries = 300;

struct Entry {
  String name;
  bool isDir;
  uint32_t size;
};

static bool endsWithIgnoreCase(const String& s, const char* ext) {
  size_t el = strlen(ext);
  if (s.length() < el) return false;
  return s.substring(s.length() - el).equalsIgnoreCase(ext);
}

static bool containsIgnoreCase(const String& hay, const String& needle) {
  if (needle.length() == 0) return true;
  String h = hay;
  String n = needle;
  h.toLowerCase();
  n.toLowerCase();
  return h.indexOf(n) >= 0;
}

static bool entryLess(const Entry& a, const Entry& b) {
  if (a.isDir != b.isDir) return a.isDir;  // folders first
  String x = a.name;
  String y = b.name;
  x.toLowerCase();
  y.toLowerCase();
  return x < y;
}

static String joinPath(const String& dir, const String& name) {
  return dir == "/" ? "/" + name : dir + "/" + name;
}

static String baseName(const String& path) {
  int i = path.lastIndexOf('/');
  return i >= 0 ? path.substring(i + 1) : path;
}

static String formatSize(uint32_t bytes) {
  char buf[12];
  if (bytes < 1024) {
    snprintf(buf, sizeof(buf), "%uB", (unsigned)bytes);
  } else if (bytes < 1024UL * 1024UL) {
    snprintf(buf, sizeof(buf), "%uK", (unsigned)(bytes / 1024));
  } else {
    snprintf(buf, sizeof(buf), "%uM", (unsigned)(bytes / (1024UL * 1024UL)));
  }
  return String(buf);
}

static void loadDir(const String& path, const char* ext,
                    std::vector<Entry>& out) {
  out.clear();
  File dir = SD.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    LOGW(TAG, "cannot open dir %s", path.c_str());
    return;
  }

  while (out.size() < kMaxEntries) {
    File f = dir.openNextFile();
    if (!f) break;

    String name = baseName(String(f.name()));
    bool isDir = f.isDirectory();
    uint32_t size = isDir ? 0 : (uint32_t)f.size();
    f.close();

    // Skip hidden entries: macOS ._ files, .Spotlight-V100, .Trashes, ...
    if (name.startsWith(".")) continue;
    if (!isDir && ext && !endsWithIgnoreCase(name, ext)) continue;

    Entry e;
    e.name = name;
    e.isDir = isDir;
    e.size = size;
    out.push_back(e);
  }
  dir.close();

  if (out.size() >= kMaxEntries) {
    LOGW(TAG, "%s: list truncated at %u entries", path.c_str(),
         (unsigned)kMaxEntries);
  }
  std::sort(out.begin(), out.end(), entryLess);
  LOGD(TAG, "%s: %u entries", path.c_str(), (unsigned)out.size());
}

bool browseFiles(const char* title, const char* startDir, const char* ext,
                 String& outPath) {
  if (!storage::mounted()) {
    ui::message(title, "No SD card", "Insert one and restart");
    return false;
  }

  String dir = (startDir && startDir[0]) ? String(startDir) : String("/");
  if (!SD.exists(dir.c_str())) dir = "/";

  std::vector<Entry> entries;   // everything in `dir`
  std::vector<int> view;        // indices into `entries` that match the filter
  std::vector<String> labels;   // text for the visible rows (kept alive here)
  std::vector<String> tags;
  std::vector<ui::MenuItem> items;
  String filter;
  int sel = 0;
  int top = 0;
  bool dirty = true;

  // Rebuild the visible rows from `entries` + `filter`.
  auto rebuild = [&]() {
    view.clear();
    for (size_t i = 0; i < entries.size(); i++) {
      if (containsIgnoreCase(entries[i].name, filter)) view.push_back((int)i);
    }

    labels.clear();
    tags.clear();
    items.clear();
    labels.reserve(view.size());
    tags.reserve(view.size());
    items.reserve(view.size());
    for (size_t k = 0; k < view.size(); k++) {
      const Entry& e = entries[view[k]];
      labels.push_back(e.isDir ? e.name + "/" : e.name);
      tags.push_back(e.isDir ? String("") : formatSize(e.size));
    }
    for (size_t k = 0; k < view.size(); k++) {
      ui::MenuItem mi;
      mi.label = labels[k].c_str();
      mi.tag = tags[k].c_str();
      items.push_back(mi);
    }

    if (sel >= (int)view.size()) sel = view.empty() ? 0 : (int)view.size() - 1;
    top = ui::keepVisible(sel, top);
    dirty = true;
  };

  // Load a folder. If `select` is given, put the cursor on that name.
  auto enter = [&](const String& newDir, const String& select) {
    dir = newDir;
    filter = "";
    loadDir(dir, ext, entries);
    sel = 0;
    top = 0;
    rebuild();
    if (select.length() > 0) {
      for (size_t k = 0; k < view.size(); k++) {
        if (entries[view[k]].name == select) {
          sel = (int)k;
          break;
        }
      }
      top = ui::keepVisible(sel, 0);
    }
  };

  enter(dir, "");

  while (true) {
    if (dirty) {
      String shown = dir;
      if (shown.length() > 20) shown = ".." + shown.substring(shown.length() - 18);
      String footer = filter.length() > 0
                          ? "~find: " + filter + "|Del clear"
                          : String("type find|UpDown move|Enter open|Del up");
      ui::drawMenu(shown.c_str(), items.data(), (int)items.size(), sel, top,
                   footer.c_str());
      dirty = false;
    }

    InputEvent ev = inputPoll();
    switch (ev.key) {
      case Key::Up:
        if (!view.empty()) {
          sel = (sel + (int)view.size() - 1) % (int)view.size();
          top = ui::keepVisible(sel, top);
          dirty = true;
        }
        break;

      case Key::Down:
        if (!view.empty()) {
          sel = (sel + 1) % (int)view.size();
          top = ui::keepVisible(sel, top);
          dirty = true;
        }
        break;

      case Key::Select:
      case Key::Right:
        if (!view.empty()) {
          const Entry& e = entries[view[sel]];
          String path = joinPath(dir, e.name);
          if (e.isDir) {
            LOGI(TAG, "open dir %s", path.c_str());
            enter(path, "");
          } else {
            LOGI(TAG, "picked %s (%u bytes)", path.c_str(), (unsigned)e.size);
            outPath = path;
            return true;
          }
        }
        break;

      case Key::Left:
        if (dir != "/") enter(storage::parentDir(dir), baseName(dir));
        break;

      case Key::Back:
        if (filter.length() > 0) {
          filter = "";
          sel = 0;
          top = 0;
          rebuild();
        } else if (dir == "/") {
          return false;
        } else {
          enter(storage::parentDir(dir), baseName(dir));
        }
        break;

      case Key::Del:
        if (filter.length() > 0) {
          filter.remove(filter.length() - 1);
          sel = 0;
          top = 0;
          rebuild();
        }
        break;

      case Key::Char:
        if (isalnum((unsigned char)ev.ch) || ev.ch == '-' || ev.ch == '_') {
          filter += ev.ch;
          sel = 0;
          top = 0;
          rebuild();
        }
        break;

      default:
        break;
    }
    delay(10);
  }
}
