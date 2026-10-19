#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <sqlite3.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits.h>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ============================================================================
// Base64, JSON & System Utilities
// ============================================================================

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Chars[(n >> 18) & 63]);
        out.push_back(kBase64Chars[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? kBase64Chars[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kBase64Chars[n & 63] : '=');
    }
    return out;
}

static std::vector<uint8_t> base64Decode(const std::string &in) {
    static int T[256];
    static bool initialized = false;
    if (!initialized) {
        std::fill(std::begin(T), std::end(T), -1);
        for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(kBase64Chars[i])] = i;
        initialized = true;
    }
    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

static std::string urlEncode(const std::string &s) {
    std::ostringstream escaped;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            escaped << buf;
        }
    }
    return escaped.str();
}

// Scrub secret-looking values before writing tool args to logs.
static std::string sanitizeArgsForLog(const std::string &argsJson) {
    std::string out = argsJson;
    std::string lower = argsJson;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char *secretKeys[] = {"password", "passwd", "secret", "token", "api_key", "apikey", "auth"};
    for (const char *key : secretKeys) {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = 0;
        while ((pos = lower.find(needle, pos)) != std::string::npos) {
            size_t colon = lower.find(':', pos + needle.size());
            if (colon == std::string::npos) break;
            size_t vstart = lower.find_first_not_of(" \t", colon + 1);
            if (vstart == std::string::npos) break;
            size_t vend = vstart;
            if (lower[vstart] == '"') {
                vend = lower.find('"', vstart + 1);
                if (vend == std::string::npos) break;
                vend++;
            } else {
                vend = lower.find_first_of(",}", vstart);
                if (vend == std::string::npos) vend = lower.size();
            }
            out.replace(vstart, vend - vstart, "\"***\"");
            lower.replace(vstart, vend - vstart, "\"***\"");
            pos = vend;
        }
    }
    return out;
}

static std::string getHomeDir() {
    const char *home = getenv("HOME");
    return (home && *home) ? std::string(home) : ".";
}

static void mkdirRecursive(const std::string &path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' && current.size() > 1) {
            mkdir(current.c_str(), 0755);
        }
    }
    if (!current.empty()) {
        mkdir(current.c_str(), 0755);
    }
}

static std::string sanitizeFilename(const std::string &title) {
    std::string out;
    for (unsigned char c : title) {
        if (isalnum(c) || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ' || c == '/' || c == ':') {
            if (out.empty() || out.back() != '_') out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "Untitled_Note";
    if (out.size() > 80) out = out.substr(0, 80);
    return out;
}

static bool envFlagEnabled(const char *name) {
    const char *v = std::getenv(name);
    if (!v) return false;
    return strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0;
}

// Safety gate: dangerous capability disabled unless the user explicitly opted in.
static std::string gatedError(const char *flag, const std::string &capability) {
    return std::string("{\"status\":\"blocked\",\"error\":\"safety_gate\",\"message\":\"") +
           capability + " is disabled by default. To enable it, restart Amuse with " + flag +
           "=1 in the environment. This capability can execute arbitrary actions on your machine, so enable it only if you accept that risk.\"}";
}

static std::string getNotesDir() {
    std::string home = getHomeDir();
    std::string notesDir = home + "/Documents/AmuseNotes";
    mkdirRecursive(notesDir);
    std::string symlinkPath = home + "/Notes";
    struct stat st{};
    if (lstat(symlinkPath.c_str(), &st) != 0) {
        int rc = symlink(notesDir.c_str(), symlinkPath.c_str());
        (void)rc;
    }
    return notesDir;
}

static std::string getPicturesDir() {
    std::string dir = getHomeDir() + "/Pictures/Amuse";
    mkdirRecursive(dir);
    return dir;
}

static std::string getVideosDir() {
    std::string dir = getHomeDir() + "/Videos/Amuse";
    mkdirRecursive(dir);
    return dir;
}

static std::string getRepoRootDir() {
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        std::string p(exePath);
        size_t pos = p.rfind("/bin/");
        if (pos != std::string::npos) {
            return p.substr(0, pos);
        }
    }
    char cwdBuf[PATH_MAX];
    return getcwd(cwdBuf, sizeof(cwdBuf)) ? std::string(cwdBuf) : ".";
}

static void spawnDetachedProcess(const std::vector<std::string> &args) {
    if (args.empty()) return;
    std::thread([args]() {
        // Double-fork so the child is reparented to init and never becomes a zombie
        pid_t pid = fork();
        if (pid == 0) {
            pid_t pid2 = fork();
            if (pid2 != 0) _exit(0); // first child exits; grandchild reparents
            if (pid2 < 0) _exit(1);
            setsid();
            int devNull = open("/dev/null", O_RDWR);
            if (devNull >= 0) {
                dup2(devNull, STDIN_FILENO);
                dup2(devNull, STDOUT_FILENO);
                dup2(devNull, STDERR_FILENO);
                close(devNull);
            }
            std::vector<char *> argv;
            for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(0);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0); // first child exits immediately
        }
    }).detach();
}

// Like spawnDetachedProcess, but writes `stdinData` to the child's stdin.
static void spawnDetachedProcessWithStdin(const std::vector<std::string> &args, const std::string &stdinData) {
    if (args.empty()) return;
    int pipeFds[2];
    if (pipe(pipeFds) != 0) return;
    std::thread([args, stdinData, pipeFds]() {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(pipeFds[0], STDIN_FILENO);
            close(pipeFds[0]);
            close(pipeFds[1]);
            int devNull = open("/dev/null", O_RDWR);
            if (devNull >= 0) {
                dup2(devNull, STDOUT_FILENO);
                dup2(devNull, STDERR_FILENO);
                close(devNull);
            }
            std::vector<char *> argv;
            for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(0);
        }
        close(pipeFds[0]);
        if (pid > 0) {
            size_t off = 0;
            while (off < stdinData.size()) {
                ssize_t w = write(pipeFds[1], stdinData.data() + off, stdinData.size() - off);
                if (w <= 0) break;
                off += static_cast<size_t>(w);
            }
            close(pipeFds[1]);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        } else {
            close(pipeFds[1]);
        }
    }).detach();
}

// Run a process with args (no shell), write stdinData to its stdin, capture
// stdout (capped), with a hard timeout. Used for credential-bearing calls so
// secrets never appear in argv (visible in `ps`).
static std::string runProcessWithIO(const std::vector<std::string> &args, const std::string &stdinData,
                                    size_t maxOut, int timeoutSec) {
    if (args.empty()) return "";
    int inPipe[2], outPipe[2];
    if (pipe(inPipe) != 0) return "";
    if (pipe(outPipe) != 0) { close(inPipe[0]); close(inPipe[1]); return ""; }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) { dup2(devNull, STDERR_FILENO); close(devNull); }
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        std::vector<char *> argv;
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    if (pid < 0) { close(inPipe[1]); close(outPipe[0]); return ""; }
    // Non-blocking read end so the timeout is actually enforced
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);

    // Feed stdin (ignore SIGPIPE if child exits early)
    signal(SIGPIPE, SIG_IGN);
    size_t off = 0;
    while (off < stdinData.size()) {
        ssize_t w = write(inPipe[1], stdinData.data() + off, stdinData.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    close(inPipe[1]);

    std::string out;
    out.reserve(4096);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool killed = false;
    while (true) {
        char buf[4096];
        ssize_t r = read(outPipe[0], buf, sizeof(buf));
        if (r > 0) {
            out.append(buf, static_cast<size_t>(r));
            if (out.size() >= maxOut) { out.resize(maxOut); break; }
            continue;
        }
        if (r == 0) break; // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (std::chrono::steady_clock::now() > deadline) { killed = true; break; }
            struct timeval tv{0, 100000};
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(outPipe[0], &rfds);
            select(outPipe[0] + 1, &rfds, nullptr, nullptr, &tv);
            continue;
        }
        break;
    }
    close(outPipe[0]);
    if (killed) {
        kill(pid, SIGKILL);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return out;
}

// Paths the agent may share outward (email attachments, uploads): inside the
// user's home, excluding sensitive locations.
static bool isPathAllowedForSharing(const std::string &rawPath) {
    if (rawPath.empty()) return true;
    std::string p = rawPath;
    if (!p.empty() && p[0] == '~') p = getHomeDir() + p.substr(1);
    char resolved[PATH_MAX];
    if (!realpath(p.c_str(), resolved)) return false;
    std::string path(resolved);
    std::string home = getHomeDir();
    if (path == home || path.rfind(home + "/", 0) != 0) return false;
    static const char *deny[] = {
        "/.ssh", "/.gnupg", "/.aws", "/.config/gcloud", "/.amuse", "/.kube",
        "/.docker", "/.local/share/keyrings", "/Library"
    };
    std::string rel = path.substr(home.size());
    for (const char *d : deny) {
        if (rel == d || rel.rfind(std::string(d) + "/", 0) == 0) return false;
    }
    return true;
}

static void sendDesktopNotification(const std::string &title, const std::string &body, bool critical = false) {
    std::vector<std::string> args = {"notify-send", "-a", "Amuse"};
    if (critical) {
        args.push_back("-u");
        args.push_back("critical");
    }
    args.push_back(title);
    args.push_back(body);
    spawnDetachedProcess(args);
}

static void openOnDesktop(const std::string &pathOrUrl) {
    if (pathOrUrl.empty()) return;
    spawnDetachedProcess({"xdg-open", pathOrUrl});
}

static void copyToClipboard(const std::string &text) {
    if (gtk_main_level() > 0) {
        std::string *heapText = new std::string(text);
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            +[](gpointer data) -> gboolean {
                std::string *s = static_cast<std::string *>(data);
                if (gdk_display_get_default()) {
                    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                    if (cb) {
                        gtk_clipboard_set_text(cb, s->c_str(), static_cast<gint>(s->size()));
                        gtk_clipboard_store(cb);
                    }
                }
                delete s;
                return G_SOURCE_REMOVE;
            },
            heapText,
            nullptr
        );
    } else {
        // Pass the text via stdin so quotes/backslashes in the text can't inject code
        std::string py = "import gi, sys; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk, Gdk, GLib; "
                         "text = sys.stdin.read(); "
                         "cb = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD); "
                         "cb.set_text(text, -1); cb.store(); "
                         "GLib.timeout_add(4000, Gtk.main_quit); Gtk.main()";
        spawnDetachedProcessWithStdin({"/usr/bin/python3", "-c", py}, text);
    }
}

static void copyImageToClipboard(const std::string &pngPath) {
    if (pngPath.empty()) return;
    if (gtk_main_level() > 0) {
        std::string *heapPath = new std::string(pngPath);
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            +[](gpointer data) -> gboolean {
                std::string *p = static_cast<std::string *>(data);
                if (gdk_display_get_default()) {
                    GError *err = nullptr;
                    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(p->c_str(), &err);
                    if (pixbuf) {
                        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                        if (cb) {
                            gtk_clipboard_set_image(cb, pixbuf);
                            gtk_clipboard_store(cb);
                        }
                        g_object_unref(pixbuf);
                    }
                    if (err) g_error_free(err);
                }
                delete p;
                return G_SOURCE_REMOVE;
            },
            heapPath,
            nullptr
        );
    } else {
        std::string py = "import gi, sys; gi.require_version('Gtk', '3.0'); "
                         "from gi.repository import Gtk, Gdk, GdkPixbuf, GLib; "
                         "pb = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1]); "
                         "cb = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD); "
                         "cb.set_image(pb); cb.store(); "
                         "GLib.timeout_add(12000, Gtk.main_quit); Gtk.main()";
        spawnDetachedProcess({"/usr/bin/python3", "-c", py, pngPath});
    }
}

static bool x11MoveAndClick(int x, int y, int button = 1, bool doClick = true) {
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
    XFlush(dpy);
    if (doClick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        XTestFakeButtonEvent(dpy, static_cast<unsigned int>(button), True, CurrentTime);
        XFlush(dpy);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        XTestFakeButtonEvent(dpy, static_cast<unsigned int>(button), False, CurrentTime);
        XFlush(dpy);
    }
    XCloseDisplay(dpy);
    return true;
}

static bool x11SendKeyCombo(const std::string &comboStr) {
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;

    std::string lower = comboStr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    bool useCtrl = (lower.find("ctrl") != std::string::npos || lower.find("control") != std::string::npos);
    bool useShift = (lower.find("shift") != std::string::npos);
    bool useAlt = (lower.find("alt") != std::string::npos);
    bool useSuper = (lower.find("super") != std::string::npos || lower.find("meta") != std::string::npos);

    KeySym mainSym = XK_VoidSymbol;
    if (lower.find("return") != std::string::npos || lower.find("enter") != std::string::npos) mainSym = XK_Return;
    else if (lower.find("tab") != std::string::npos) mainSym = XK_Tab;
    else if (lower.find("esc") != std::string::npos) mainSym = XK_Escape;
    else if (lower.find("space") != std::string::npos) mainSym = XK_space;
    else if (lower.find("backspace") != std::string::npos) mainSym = XK_BackSpace;
    else {
        size_t plus = comboStr.rfind('+');
        std::string keyPart = (plus != std::string::npos) ? comboStr.substr(plus + 1) : comboStr;
        while (!keyPart.empty() && keyPart.front() == ' ') keyPart.erase(0, 1);
        if (keyPart.size() == 1) {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(keyPart[0])));
            mainSym = XStringToKeysym(std::string(1, c).c_str());
        } else {
            mainSym = XStringToKeysym(keyPart.c_str());
        }
    }

    KeyCode ctrlCode = useCtrl ? XKeysymToKeycode(dpy, XK_Control_L) : 0;
    KeyCode shiftCode = useShift ? XKeysymToKeycode(dpy, XK_Shift_L) : 0;
    KeyCode altCode = useAlt ? XKeysymToKeycode(dpy, XK_Alt_L) : 0;
    KeyCode superCode = useSuper ? XKeysymToKeycode(dpy, XK_Super_L) : 0;
    KeyCode mainCode = (mainSym != XK_VoidSymbol) ? XKeysymToKeycode(dpy, mainSym) : 0;

    if (ctrlCode) XTestFakeKeyEvent(dpy, ctrlCode, True, CurrentTime);
    if (shiftCode) XTestFakeKeyEvent(dpy, shiftCode, True, CurrentTime);
    if (altCode) XTestFakeKeyEvent(dpy, altCode, True, CurrentTime);
    if (superCode) XTestFakeKeyEvent(dpy, superCode, True, CurrentTime);
    XFlush(dpy);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    if (mainCode) {
        XTestFakeKeyEvent(dpy, mainCode, True, CurrentTime);
        XFlush(dpy);
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        XTestFakeKeyEvent(dpy, mainCode, False, CurrentTime);
        XFlush(dpy);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (superCode) XTestFakeKeyEvent(dpy, superCode, False, CurrentTime);
    if (altCode) XTestFakeKeyEvent(dpy, altCode, False, CurrentTime);
    if (shiftCode) XTestFakeKeyEvent(dpy, shiftCode, False, CurrentTime);
    if (ctrlCode) XTestFakeKeyEvent(dpy, ctrlCode, False, CurrentTime);
    XFlush(dpy);
    XCloseDisplay(dpy);
    return mainCode != 0;
}

static std::string runCommandCapture(const std::string &cmd, int maxBytes = 16384) {
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    std::string out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp) != nullptr) {
        out += buf;
        if (static_cast<int>(out.size()) >= maxBytes) break;
    }
    pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static std::string shellSingleQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

// Pure C++ thread-safe top-level JSON object parser
struct JsonHelper {
    struct Entry {
        std::string strVal;
        std::string rawJson;
        bool isString = false;
    };
    bool valid = false;
    std::map<std::string, Entry> fields;

    JsonHelper(const std::string &jsonStr) {
        parse(jsonStr);
    }

    bool isValidObject() const {
        return valid;
    }

    std::string getString(const char *key, const std::string &def = "") const {
        auto it = fields.find(key);
        if (it == fields.end() || it->second.rawJson == "null") return def;
        return it->second.isString ? it->second.strVal : it->second.rawJson;
    }

    int getInt(const char *key, int def = 0) const {
        auto it = fields.find(key);
        if (it == fields.end() || it->second.rawJson == "null") return def;
        const std::string &s = it->second.isString ? it->second.strVal : it->second.rawJson;
        if (s.empty()) return def;
        return std::atoi(s.c_str());
    }

    bool getBool(const char *key, bool def = false) const {
        auto it = fields.find(key);
        if (it == fields.end() || it->second.rawJson == "null") return def;
        const std::string &s = it->second.isString ? it->second.strVal : it->second.rawJson;
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
        return def;
    }

    std::string getObjectJson(const char *key, const std::string &def = "{}") const {
        auto it = fields.find(key);
        if (it == fields.end() || it->second.rawJson == "null" || it->second.rawJson.empty()) return def;
        return it->second.rawJson;
    }

private:
    static void skipWs(const std::string &s, size_t &i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }

    static bool parseString(const std::string &s, size_t &i, std::string &out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\' && i < s.size()) {
                char esc = s[i++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (i + 4 <= s.size()) {
                            unsigned int cp = std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                            i += 4;
                            if (cp < 0x80) {
                                out.push_back(static_cast<char>(cp));
                            } else if (cp < 0x800) {
                                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                        }
                        break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    void parse(const std::string &s) {
        size_t i = 0;
        skipWs(s, i);
        if (i >= s.size() || s[i] != '{') return;
        i++;
        valid = true;

        while (i < s.size()) {
            skipWs(s, i);
            if (i < s.size() && s[i] == '}') {
                i++;
                return;
            }
            std::string key;
            if (!parseString(s, i, key)) return;
            skipWs(s, i);
            if (i >= s.size() || s[i] != ':') return;
            i++;
            skipWs(s, i);
            if (i >= s.size()) return;

            Entry entry;
            size_t start = i;
            if (s[i] == '"') {
                entry.isString = true;
                parseString(s, i, entry.strVal);
                entry.rawJson = s.substr(start, i - start);
            } else if (s[i] == '{' || s[i] == '[') {
                int depth = 0;
                bool inStr = false;
                while (i < s.size()) {
                    char c = s[i++];
                    if (inStr) {
                        if (c == '\\' && i < s.size()) i++;
                        else if (c == '"') inStr = false;
                    } else {
                        if (c == '"') inStr = true;
                        else if (c == '{' || c == '[') depth++;
                        else if (c == '}' || c == ']') {
                            depth--;
                            if (depth == 0) break;
                        }
                    }
                }
                entry.rawJson = s.substr(start, i - start);
            } else {
                while (i < s.size() && s[i] != ',' && s[i] != '}' &&
                       s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
                    i++;
                }
                entry.rawJson = s.substr(start, i - start);
            }
            fields[key] = std::move(entry);

            skipWs(s, i);
            if (i < s.size() && s[i] == ',') i++;
        }
    }
};

// ============================================================================
// PulseAudio / PipeWire Native Audio Engine (16kHz in / 24kHz out + Alarm Chime)
// ============================================================================

class LinuxAudioEngine {
public:
    using ChunkCallback = std::function<void(const uint8_t *, size_t, float)>;
    using PlaybackStateCallback = std::function<void(bool)>;

    static LinuxAudioEngine &instance() {
        static LinuxAudioEngine inst;
        return inst;
    }

    bool startRecording(ChunkCallback cb, std::string &errOut) {
        stopRecording();

        pa_sample_spec ss;
        ss.format = PA_SAMPLE_S16LE;
        ss.rate = 16000;
        ss.channels = 1;

        pa_buffer_attr attr;
        memset(&attr, 0xFF, sizeof(attr));
        attr.fragsize = 4096; // 2048 samples = 128ms

        int error = 0;
        pa_simple *rec = pa_simple_new(
            nullptr, "Amuse", PA_STREAM_RECORD, nullptr,
            "Amuse Voice Capture", &ss, nullptr, &attr, &error
        );
        if (!rec) {
            errOut = pa_strerror(error);
            return false;
        }

        recording_ = true;
        recordThread_ = std::thread([this, rec, cb]() {
            std::vector<int16_t> buf(2048);
            int err = 0;
            while (recording_) {
                if (pa_simple_read(rec, buf.data(), buf.size() * sizeof(int16_t), &err) < 0) {
                    break;
                }
                if (!recording_) break;
                double sum = 0.0;
                for (int16_t sample : buf) {
                    sum += static_cast<double>(sample) * static_cast<double>(sample);
                }
                float rms = std::sqrt(sum / buf.size()) / 32768.0f;
                float level = std::min(1.0f, rms * 4.0f);
                cb(reinterpret_cast<const uint8_t *>(buf.data()), buf.size() * sizeof(int16_t), level);
            }
            pa_simple_free(rec);
        });
        return true;
    }

    void stopRecording() {
        recording_ = false;
        if (recordThread_.joinable()) {
            recordThread_.join();
        }
    }

    void enqueuePlayback(const std::vector<uint8_t> &pcm24k, PlaybackStateCallback stateCb = nullptr) {
        if (pcm24k.empty()) return;
        ensurePlaybackThread();
        {
            std::lock_guard<std::mutex> lock(playMutex_);
            if (stateCb) playbackStateCb_ = stateCb;
            playQueue_.push(pcm24k);
        }
        playCv_.notify_one();
    }

    void playAlarmChime() {
        const int sampleRate = 24000;
        const double freqs[] = {523.25, 659.25, 783.99, 1046.50, 783.99, 1046.50};
        const int noteSamples = sampleRate / 5; // 200ms per note
        std::vector<int16_t> samples;
        samples.reserve(noteSamples * 6);

        for (double freq : freqs) {
            for (int i = 0; i < noteSamples; ++i) {
                double t = static_cast<double>(i) / sampleRate;
                double env = std::exp(-4.5 * (static_cast<double>(i) / noteSamples));
                double wave = std::sin(2.0 * M_PI * freq * t) + 0.3 * std::sin(4.0 * M_PI * freq * t);
                int32_t val = static_cast<int32_t>(wave * env * 12000.0);
                val = std::max(-32767, std::min(32767, val));
                samples.push_back(static_cast<int16_t>(val));
            }
        }

        std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t));
        memcpy(bytes.data(), samples.data(), bytes.size());
        enqueuePlayback(bytes);
    }

private:
    std::atomic<bool> recording_{false};
    std::thread recordThread_;

    std::atomic<bool> playThreadRunning_{false};
    std::thread playThread_;
    std::mutex playMutex_;
    std::condition_variable playCv_;
    std::queue<std::vector<uint8_t>> playQueue_;
    PlaybackStateCallback playbackStateCb_;

    LinuxAudioEngine() = default;
    ~LinuxAudioEngine() {
        stopRecording();
        playThreadRunning_ = false;
        playCv_.notify_all();
        if (playThread_.joinable()) playThread_.join();
    }

    void ensurePlaybackThread() {
        bool expected = false;
        if (!playThreadRunning_.compare_exchange_strong(expected, true)) return;

        playThread_ = std::thread([this]() {
            pa_sample_spec ss;
            ss.format = PA_SAMPLE_S16LE;
            ss.rate = 24000;
            ss.channels = 1;

            int err = 0;
            pa_simple *play = pa_simple_new(
                nullptr, "Amuse", PA_STREAM_PLAYBACK, nullptr,
                "Amuse Gemini Voice", &ss, nullptr, nullptr, &err
            );

            bool wasPlaying = false;
            while (playThreadRunning_) {
                std::vector<uint8_t> chunk;
                PlaybackStateCallback cb;
                {
                    std::unique_lock<std::mutex> lock(playMutex_);
                    if (playQueue_.empty()) {
                        if (wasPlaying) {
                            wasPlaying = false;
                            cb = playbackStateCb_;
                            lock.unlock();
                            if (cb) cb(false);
                            lock.lock();
                        }
                        playCv_.wait_for(lock, std::chrono::milliseconds(250), [this]() {
                            return !playQueue_.empty() || !playThreadRunning_;
                        });
                        if (!playThreadRunning_) break;
                        if (playQueue_.empty()) continue;
                    }
                    chunk = std::move(playQueue_.front());
                    playQueue_.pop();
                    cb = playbackStateCb_;
                }

                if (!wasPlaying) {
                    wasPlaying = true;
                    if (cb) cb(true);
                }

                if (play && !chunk.empty()) {
                    pa_simple_write(play, chunk.data(), chunk.size(), &err);
                }
            }

            if (play) {
                pa_simple_drain(play, &err);
                pa_simple_free(play);
            }
        });
    }
};

// ============================================================================
// Local & Cloud Connectors Manager (SQLite3 + Desktop + Muse Integrations)
// ============================================================================

struct RichCardEvent {
    std::string cardType;
    std::string title;
    std::string badge;
    std::string subtitle;
    std::string body;
    std::string filePath;
    std::string actionLabel;
};

class LocalConnectorsManager {
public:
    using RichCardCallback = std::function<void(const RichCardEvent &)>;
    using ImageFrameCallback = std::function<void(const std::string &base64Data, const std::string &mimeType)>;

    static LocalConnectorsManager &instance() {
        static LocalConnectorsManager inst;
        return inst;
    }

    void setRichCardCallback(RichCardCallback cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        richCardCb_ = cb;
    }

    void setImageFrameCallback(ImageFrameCallback cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        imageFrameCb_ = cb;
    }

    void emitRichCard(const RichCardEvent &card) {
        RichCardCallback cb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            cb = richCardCb_;
        }
        if (cb) cb(card);
    }

    void emitImageFrame(const std::string &base64Data, const std::string &mimeType = "image/png") {
        ImageFrameCallback cb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            cb = imageFrameCb_;
        }
        if (cb) cb(base64Data, mimeType);
    }

    bool hasImageFrameSink() {
        std::lock_guard<std::mutex> lock(cbMutex_);
        return static_cast<bool>(imageFrameCb_);
    }

    std::string getToolDeclarationsJson() {
        return R"JSON([
  {
    "name": "get_current_time_and_environment",
    "description": "Returns the exact current local date, time, timezone, UTC offset, times across US Pacific/Mountain/Central(CST/CDT)/Eastern and UTC, OS desktop details, local directories (~/Documents/AmuseNotes), and active connectors. Call this whenever checking current time or timezone differences.",
    "parameters": {
      "type": "OBJECT",
      "properties": {}
    }
  },
  {
    "name": "list_connectors",
    "description": "Lists all available local and cloud Muse connectors (Notes, Calendar, Reminders, Alarms, Messages, Contacts, Files, Screen/Screencast, LinkedIn, Gmail, Google Calendar, Slack, Notion, GitHub, WhatsApp, Web Search) and their status.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "category": {
          "type": "STRING",
          "description": "Optional filter: 'all', 'local', or 'cloud'"
        }
      }
    }
  },
  {
    "name": "connect_service",
    "description": "Connects, enables, or configures permissions for a local or external connector (e.g., linkedin, gmail, google_calendar, slack, notion, github, whatsapp, files_disk, computer_control, notes, calendar, reminders, alarms_timers, messages).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "service": {
          "type": "STRING",
          "description": "Connector ID or name (e.g. 'linkedin', 'gmail', 'slack', 'notion', 'github', 'whatsapp', 'google_calendar')"
        },
        "permission": {
          "type": "STRING",
          "description": "Permission level: 'read_and_write' (default) or 'read_only'"
        },
        "open_in_browser": {
          "type": "BOOLEAN",
          "description": "If true, also opens the service web app in the user's browser"
        }
      },
      "required": ["service"]
    }
  },
  {
    "name": "search_messages",
    "description": "Searches the user's local messages database for text content and/or sender telephone or email handle.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "query": {
          "type": "STRING",
          "description": "Search text keyword to find in messages"
        },
        "sender": {
          "type": "STRING",
          "description": "Optional phone number, email, or contact handle"
        },
        "limit": {
          "type": "INTEGER",
          "description": "Maximum number of messages to return (default 15)"
        }
      }
    }
  },
  {
    "name": "get_recent_messages",
    "description": "Fetches the user's most recent incoming and outgoing messages.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "limit": {
          "type": "INTEGER",
          "description": "Maximum messages to fetch (default 15)"
        }
      }
    }
  },
  {
    "name": "send_message",
    "description": "Sends a message to a recipient via local Messages, WhatsApp, or Slack connector.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "recipient": {
          "type": "STRING",
          "description": "Recipient name, phone number, or handle"
        },
        "text": {
          "type": "STRING",
          "description": "Message text content"
        },
        "channel": {
          "type": "STRING",
          "description": "Channel connector: 'messages' (default), 'whatsapp', or 'slack'"
        }
      },
      "required": ["recipient", "text"]
    }
  },
  {
    "name": "search_contacts",
    "description": "Searches local Contacts by name, phone number, or email to identify people, phone numbers, or addresses.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "name": {
          "type": "STRING",
          "description": "Contact name or partial name query"
        },
        "phone_or_email": {
          "type": "STRING",
          "description": "Phone number or email address to resolve to a contact"
        }
      }
    }
  },
  {
    "name": "create_contact",
    "description": "Creates a new contact in the user's local Contacts database.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "full_name": {
          "type": "STRING",
          "description": "Full name of the contact"
        },
        "phone_number": {
          "type": "STRING",
          "description": "Phone number"
        },
        "email_address": {
          "type": "STRING",
          "description": "Email address"
        },
        "organization": {
          "type": "STRING",
          "description": "Company or organization"
        }
      },
      "required": ["full_name"]
    }
  },
  {
    "name": "get_calendar_events",
    "description": "Retrieves the user's scheduled calendar events for the upcoming days.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "days_ahead": {
          "type": "INTEGER",
          "description": "Number of days ahead to search (default 7)"
        }
      }
    }
  },
  {
    "name": "create_calendar_event",
    "description": "Creates a new event on the user's local Calendar and syncs it with the Linux GNOME Calendar (.ics).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "title": {
          "type": "STRING",
          "description": "Title of the event"
        },
        "start_iso": {
          "type": "STRING",
          "description": "Start time in ISO-8601 format or human time string (e.g. '2026-10-13T09:00:00-05:00' or 'today 9:00 AM CST')"
        },
        "end_iso": {
          "type": "STRING",
          "description": "End time in ISO-8601 format"
        },
        "location": {
          "type": "STRING",
          "description": "Optional location"
        },
        "notes": {
          "type": "STRING",
          "description": "Optional notes or description"
        }
      },
      "required": ["title", "start_iso"]
    }
  },
  {
    "name": "get_reminders",
    "description": "Retrieves the user's to-do items from Reminders / Tasks.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "include_completed": {
          "type": "BOOLEAN",
          "description": "Whether to include completed tasks (default false)"
        }
      }
    }
  },
  {
    "name": "create_reminder",
    "description": "Creates a new to-do reminder and syncs it with Linux GNOME Tasks (.ics).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "title": {
          "type": "STRING",
          "description": "Reminder task text"
        },
        "due_iso": {
          "type": "STRING",
          "description": "Optional due date/time in ISO-8601 or natural format (e.g. '09:00 CST')"
        },
        "notes": {
          "type": "STRING",
          "description": "Optional notes"
        }
      },
      "required": ["title"]
    }
  },
  {
    "name": "set_alarm_or_timer",
    "description": "Sets a real system alarm or countdown timer on the user's Linux machine with an audio chime and critical desktop notification.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "label": {
          "type": "STRING",
          "description": "Alarm or timer label (e.g. 'LinkedIn Post & Screencast', 'Morning Alarm')"
        },
        "target_time": {
          "type": "STRING",
          "description": "Target clock time (e.g. '09:00 CST', '07:00 PDT', '2026-10-13T09:00:00-05:00'). Optional if duration_seconds is given."
        },
        "duration_seconds": {
          "type": "INTEGER",
          "description": "Countdown duration in seconds (e.g. 60 for 1 minute). Optional if target_time is given."
        }
      },
      "required": ["label"]
    }
  },
  {
    "name": "list_alarms_and_timers",
    "description": "Lists all active and scheduled alarms, timers, and scheduled tasks.",
    "parameters": {
      "type": "OBJECT",
      "properties": {}
    }
  },
  {
    "name": "search_notes",
    "description": "Searches the user's local Notes (saved in SQLite and as Markdown files in ~/Documents/AmuseNotes/ and ~/Notes/).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "query": {
          "type": "STRING",
          "description": "Search keyword or title"
        },
        "limit": {
          "type": "INTEGER",
          "description": "Max notes to return (default 10)"
        }
      }
    }
  },
  {
    "name": "create_note",
    "description": "Creates a new note in local SQLite AND writes a real Markdown (.md) file in ~/Documents/AmuseNotes/ (also accessible at ~/Notes/) and shows it as a card in the Amuse HUD.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "title": {
          "type": "STRING",
          "description": "Title of the note"
        },
        "body": {
          "type": "STRING",
          "description": "Body text or Markdown content of the note"
        },
        "open_on_desktop": {
          "type": "BOOLEAN",
          "description": "If true, also opens the created .md note file in the user's Linux desktop viewer/editor"
        }
      },
      "required": ["title", "body"]
    }
  },
  {
    "name": "open_note",
    "description": "Opens an existing note by title or keyword in the Amuse HUD and on the user's Linux desktop (xdg-open on ~/Documents/AmuseNotes/<Title>.md).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "query": {
          "type": "STRING",
          "description": "Title or search keyword of the note to open (leave empty to open the most recent note)"
        },
        "open_on_desktop": {
          "type": "BOOLEAN",
          "description": "Whether to also launch the Linux desktop file viewer via xdg-open (default true)"
        }
      }
    }
  },
  {
    "name": "social_media_action",
    "description": "End-to-end social media & website posting connector (LinkedIn, X/Twitter, Reddit, Facebook, HackerNews, or custom URL). Automatically captures/attaches a desktop screenshot when requested (capture_screenshot=true or image_path), copies the screenshot PNG image or post text to the X11 clipboard, opens the live composer in the user's logged-in browser with the post text pre-populated, and uses X11 XTest keyboard automation to paste the attached screenshot (Ctrl+V) and optionally submit (Ctrl+Enter).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "platform": {
          "type": "STRING",
          "description": "Social platform or website: 'linkedin' (default), 'twitter_x', 'reddit', 'facebook', 'hackernews'"
        },
        "action": {
          "type": "STRING",
          "description": "Action: 'post_now' (default), 'open_composer', 'schedule', or 'draft'"
        },
        "content": {
          "type": "STRING",
          "description": "Full post text to publish"
        },
        "capture_screenshot": {
          "type": "BOOLEAN",
          "description": "If true, captures a desktop screenshot (or attaches the just-captured screenshot) and pastes the PNG image directly into the post composer via X11 clipboard + Ctrl+V"
        },
        "image_path": {
          "type": "STRING",
          "description": "Optional path to an image/screenshot to attach (or 'latest' to attach the latest screenshot)"
        },
        "auto_submit": {
          "type": "BOOLEAN",
          "description": "If true, also sends Ctrl+Enter via X11 XTest after pasting to submit the post automatically"
        },
        "schedule_time": {
          "type": "STRING",
          "description": "Optional time to schedule the post for (e.g. '09:00 CST' or ISO-8601)"
        },
        "open_in_browser": {
          "type": "BOOLEAN",
          "description": "Whether to open the live post composer in the browser immediately (default true)"
        }
      },
      "required": ["content"]
    }
  },
  {
    "name": "email_action",
    "description": "Searches, reads, drafts, or sends real emails across the user's on-device Thunderbird email client AND any connected hosted IMAP/SMTP accounts (~/.amuse/email_accounts.json). Sending requires confirm_send: true after showing the user the exact draft.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'search' (or 'check'), 'draft' (or 'compose'), or 'send'"
        },
        "query": {
          "type": "STRING",
          "description": "Optional search keyword or subject filter when action is 'search'"
        },
        "to": {
          "type": "STRING",
          "description": "Recipient email address or sender filter"
        },
        "subject": {
          "type": "STRING",
          "description": "Email subject line or search query"
        },
        "body": {
          "type": "STRING",
          "description": "Email body content"
        },
        "attachment_path": {
          "type": "STRING",
          "description": "Optional file path to attach (e.g. screenshot or note)"
        },
        "open_mail_client": {
          "type": "BOOLEAN",
          "description": "If true, launches the on-device Thunderbird compose window or Gmail Web compose"
        }
      },
      "required": ["action"]
    }
  },
  {
    "name": "connect_email_account",
    "description": "Connects and authenticates a hosted IMAP/SMTP email account (Gmail App Password, Office365, or custom IMAP/SMTP) and saves credentials securely in ~/.amuse/email_accounts.json.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "email": {
          "type": "STRING",
          "description": "Email address to connect (e.g. user@gmail.com)"
        },
        "password": {
          "type": "STRING",
          "description": "App Password or IMAP/SMTP password"
        },
        "provider": {
          "type": "STRING",
          "description": "'gmail', 'outlook', or 'custom'"
        },
        "imap_host": {
          "type": "STRING",
          "description": "Optional IMAP host (default imap.gmail.com)"
        },
        "smtp_host": {
          "type": "STRING",
          "description": "Optional SMTP host (default smtp.gmail.com)"
        }
      },
      "required": ["email", "password"]
    }
  },
  {
    "name": "record_screencast",
    "description": "Records an MP4 video screencast of the user's Linux desktop screen using ffmpeg (immediately or scheduled for a future time) and saves it to ~/Videos/Amuse/.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "title": {
          "type": "STRING",
          "description": "Title or filename label for the screencast"
        },
        "duration_seconds": {
          "type": "INTEGER",
          "description": "Duration in seconds to record (default 5, max 60)"
        },
        "schedule_time": {
          "type": "STRING",
          "description": "Optional future time to schedule the screencast recording (e.g. '09:00 CST')"
        }
      }
    }
  },
  {
    "name": "take_screenshot",
    "description": "Captures a PNG screenshot of the user's Linux desktop screen using ffmpeg and saves it to ~/Pictures/Amuse/.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "title": {
          "type": "STRING",
          "description": "Optional filename label for the screenshot"
        },
        "open_after_capture": {
          "type": "BOOLEAN",
          "description": "If true, opens the screenshot on the desktop"
        }
      }
    }
  },
  {
    "name": "manage_files",
    "description": "Muse Full-Disk File Assistant: list, search, read, write, organize/move, or trash files in the user's home directory (~/Documents, ~/Downloads, ~/Desktop, etc.).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "Action: 'list', 'search', 'read', 'write', 'move', or 'trash'"
        },
        "path": {
          "type": "STRING",
          "description": "Directory or file path (supports ~ expansion, default ~/Documents)"
        },
        "query": {
          "type": "STRING",
          "description": "Search pattern or destination path (for 'move') or file content (for 'write')"
        }
      },
      "required": ["action"]
    }
  },
  {
    "name": "computer_control_action",
    "description": "Controls the user's Linux desktop: open an application ('open_app'), open a URL in browser ('open_url'), copy text to clipboard ('copy_clipboard'), or run a shell command ('run_command').",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'open_app', 'open_url', 'copy_clipboard', or 'run_command'"
        },
        "target": {
          "type": "STRING",
          "description": "App name, URL, text to copy, or shell command"
        }
      },
      "required": ["action", "target"]
    }
  },
  {
    "name": "web_search_and_fetch",
    "description": "Searches the web for live information or fetches the readable text of a URL.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "query_or_url": {
          "type": "STRING",
          "description": "Web search query or https:// URL to read"
        }
      },
      "required": ["query_or_url"]
    }
  },
  {
    "name": "schedule_task",
    "description": "Schedules any background task, post, screencast, or workflow for a future time (e.g. '09:00 CST') with automatic desktop notification and alarm trigger.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "title": {
          "type": "STRING",
          "description": "Title of the scheduled task"
        },
        "task_type": {
          "type": "STRING",
          "description": "Type: 'linkedin_post', 'screencast', 'reminder', 'command', or 'general'"
        },
        "target_time": {
          "type": "STRING",
          "description": "Target time (e.g. '09:00 CST', '2026-10-13T09:00:00-05:00', or '+60 seconds')"
        },
        "details": {
          "type": "STRING",
          "description": "Payload, post content, or instructions for the scheduled task"
        }
      },
      "required": ["title", "target_time"]
    }
  },
  {
    "name": "save_memory",
    "description": "Muse Active Memory: Saves a persistent user preference, personal fact, project detail, or habit so Amuse remembers it across all future conversations.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "key": {
          "type": "STRING",
          "description": "Short topic or key (e.g. 'preferred_timezone', 'linkedin_workflow', 'current_project')"
        },
        "value": {
          "type": "STRING",
          "description": "Fact, preference, or context to remember"
        },
        "category": {
          "type": "STRING",
          "description": "Category: 'preference', 'project', 'habit', or 'personal'"
        }
      },
      "required": ["key", "value"]
    }
  },
  {
    "name": "recall_memories",
    "description": "Muse Active Memory: Retrieves stored user facts, preferences, and ongoing project context.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "query": {
          "type": "STRING",
          "description": "Optional search keyword"
        }
      }
    }
  },
  {
    "name": "manage_action_plan",
    "description": "Muse Action Plans: Creates, updates, or lists multi-step autonomous action plans for complex user goals (e.g. travel planning, launch campaigns, research projects).",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'create', 'update', or 'list'"
        },
        "goal": {
          "type": "STRING",
          "description": "High-level goal title"
        },
        "steps": {
          "type": "STRING",
          "description": "Markdown checklist or numbered steps of the plan"
        },
        "status": {
          "type": "STRING",
          "description": "'in_progress', 'completed', or 'paused'"
        }
      },
      "required": ["action"]
    }
  },
  {
    "name": "build_and_run_custom_tool",
    "description": "Muse Dynamic Tool Builder: Dynamically builds, saves, and executes a custom Python or Bash script in ~/.amuse/custom_tools/ when a task needs custom automation or processing.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "tool_name": {
          "type": "STRING",
          "description": "Name of the custom tool (e.g. 'csv_analyzer', 'price_checker')"
        },
        "language": {
          "type": "STRING",
          "description": "'python' or 'bash'"
        },
        "code": {
          "type": "STRING",
          "description": "Complete script source code to save and run"
        },
        "description": {
          "type": "STRING",
          "description": "What this custom tool does"
        }
      },
      "required": ["tool_name", "code"]
    }
  },
  {
    "name": "media_and_system_control",
    "description": "Controls Linux system audio volume, media/Spotify playback (play_pause, next, prev), active window inspection, or desktop text insertion.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'volume_up', 'volume_down', 'mute_toggle', 'media_play_pause', 'media_next', 'media_prev', 'active_window_info', or 'open_spotify'"
        }
      },
      "required": ["action"]
    }
  },
  {
    "name": "browser_agent_action",
    "description": "Muse Browser VM & Playwright Web Agent: Navigates to any URL in a real Chromium browser, extracts interactive buttons/inputs/text, fills web forms, uploads files, clicks buttons, and captures webpage screenshots.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'navigate_and_extract', 'fill_and_submit', or 'screenshot_url'"
        },
        "url": {
          "type": "STRING",
          "description": "Target webpage URL"
        },
        "fields": {
          "type": "STRING",
          "description": "Optional JSON object mapping CSS selectors/labels/placeholders to values for 'fill_and_submit'"
        },
        "submit_selector": {
          "type": "STRING",
          "description": "Optional button selector or button text to click after filling"
        },
        "file_path": {
          "type": "STRING",
          "description": "Optional file path to upload into a file input"
        }
      },
      "required": ["action", "url"]
    }
  },
  {
    "name": "gui_desktop_action",
    "description": "Native X11 GUI Desktop Control (libXtst): Moves mouse, clicks coordinates, sends keyboard shortcuts (e.g. 'Ctrl+V', 'Ctrl+Return', 'Alt+Tab'), types text into the active window, or copies an image file to clipboard and pastes it via Ctrl+V.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'click', 'move_mouse', 'hotkey', 'type_text', or 'paste_image'"
        },
        "x": {
          "type": "INTEGER",
          "description": "X screen coordinate for click or move_mouse"
        },
        "y": {
          "type": "INTEGER",
          "description": "Y screen coordinate for click or move_mouse"
        },
        "text_or_key": {
          "type": "STRING",
          "description": "Key combo (e.g. 'Ctrl+V', 'Ctrl+Return'), text to type, or image file path for 'paste_image'"
        }
      },
      "required": ["action"]
    }
  }
])JSON";
    }

    std::string executeTool(const std::string &toolName, const std::string &argsJson) {
        std::lock_guard<std::recursive_mutex> lock(dbMutex_);
        ensureDatabase();
        JsonHelper args(argsJson);

        if (toolName == "get_current_time_and_environment") {
            return getCurrentTimeAndEnvironment();
        } else if (toolName == "list_connectors") {
            return listConnectors(args.getString("category", "all"));
        } else if (toolName == "connect_service") {
            return connectService(
                args.getString("service", ""),
                args.getString("permission", "read_and_write"),
                args.getBool("open_in_browser", false)
            );
        } else if (toolName == "search_messages") {
            autoConnect("messages");
            int limit = args.getInt("limit", 15);
            if (limit <= 0) limit = 15;
            return searchMessages(args.getString("query", ""), args.getString("sender", ""), limit);
        } else if (toolName == "get_recent_messages") {
            autoConnect("messages");
            int limit = args.getInt("limit", 15);
            if (limit <= 0) limit = 15;
            return searchMessages("", "", limit);
        } else if (toolName == "send_message") {
            std::string channel = args.getString("channel", "messages");
            autoConnect(channel);
            return sendMessage(args.getString("recipient", ""), args.getString("text", ""), channel);
        } else if (toolName == "search_contacts") {
            autoConnect("contacts");
            return searchContacts(args.getString("name", ""), args.getString("phone_or_email", ""));
        } else if (toolName == "create_contact") {
            autoConnect("contacts");
            return createContact(
                args.getString("full_name", ""),
                args.getString("phone_number", ""),
                args.getString("email_address", ""),
                args.getString("organization", "")
            );
        } else if (toolName == "get_calendar_events") {
            autoConnect("calendar");
            int daysAhead = args.getInt("days_ahead", 7);
            if (daysAhead <= 0) daysAhead = 7;
            return getCalendarEvents(daysAhead);
        } else if (toolName == "create_calendar_event") {
            autoConnect("calendar");
            return createCalendarEvent(
                args.getString("title", "Untitled Event"),
                args.getString("start_iso", currentIsoTimestamp()),
                args.getString("end_iso", ""),
                args.getString("location", ""),
                args.getString("notes", "")
            );
        } else if (toolName == "get_reminders") {
            autoConnect("reminders");
            return getReminders(args.getBool("include_completed", false));
        } else if (toolName == "create_reminder") {
            autoConnect("reminders");
            return createReminder(
                args.getString("title", "Reminder"),
                args.getString("due_iso", ""),
                args.getString("notes", "")
            );
        } else if (toolName == "set_alarm_or_timer") {
            autoConnect("alarms_timers");
            return setAlarmOrTimer(
                args.getString("label", "Alarm"),
                args.getString("target_time", ""),
                args.getInt("duration_seconds", 0)
            );
        } else if (toolName == "list_alarms_and_timers") {
            autoConnect("alarms_timers");
            return listAlarmsAndTimers();
        } else if (toolName == "search_notes") {
            autoConnect("notes");
            int limit = args.getInt("limit", 10);
            if (limit <= 0) limit = 10;
            return searchNotes(args.getString("query", ""), limit);
        } else if (toolName == "create_note") {
            autoConnect("notes");
            return createNote(
                args.getString("title", "New Note"),
                args.getString("body", ""),
                args.getBool("open_on_desktop", false)
            );
        } else if (toolName == "open_note") {
            autoConnect("notes");
            return openNote(
                args.getString("query", ""),
                args.getBool("open_on_desktop", true)
            );
        } else if (toolName == "social_media_action") {
            std::string platform = args.getString("platform", "linkedin");
            autoConnect(platform);
            std::string schedTime = args.getString("schedule_time", "");
            bool autoSubmit = args.getBool("auto_submit", false);
            if (autoSubmit && !envFlagEnabled("AMUSE_ALLOW_AUTO_SUBMIT")) {
                return gatedError("AMUSE_ALLOW_AUTO_SUBMIT", "Auto-submitting social media posts");
            }
            return socialMediaAction(
                platform,
                args.getString("action", schedTime.empty() ? "post_now" : "schedule"),
                args.getString("content", ""),
                schedTime,
                args.getBool("open_in_browser", schedTime.empty()),
                args.getBool("capture_screenshot", false),
                args.getString("image_path", ""),
                args.getBool("auto_submit", false)
            );
        } else if (toolName == "email_action") {
            autoConnect("gmail");
            return emailAction(argsJson);
        } else if (toolName == "connect_email_account") {
            autoConnect("gmail");
            return connectEmailAccount(argsJson);
        } else if (toolName == "record_screencast") {
            autoConnect("computer_control");
            return recordScreencast(
                args.getString("title", "Screencast"),
                args.getInt("duration_seconds", 5),
                args.getString("schedule_time", "")
            );
        } else if (toolName == "take_screenshot") {
            autoConnect("computer_control");
            return takeScreenshot(
                args.getString("title", "Screenshot"),
                args.getBool("open_after_capture", false)
            );
        } else if (toolName == "manage_files") {
            autoConnect("files_disk");
            return manageFiles(
                args.getString("action", "list"),
                args.getString("path", "~/Documents"),
                args.getString("query", "")
            );
        } else if (toolName == "computer_control_action") {
            autoConnect("computer_control");
            std::string ccAction = args.getString("action", "");
            if (ccAction == "run_command" && !envFlagEnabled("AMUSE_ALLOW_SHELL")) {
                return gatedError("AMUSE_ALLOW_SHELL", "Running arbitrary shell commands");
            }
            return computerControlAction(
                ccAction,
                args.getString("target", "")
            );
        } else if (toolName == "web_search_and_fetch") {
            autoConnect("web_browser");
            return webSearchAndFetch(args.getString("query_or_url", ""));
        } else if (toolName == "schedule_task") {
            autoConnect("alarms_timers");
            return scheduleTask(
                args.getString("title", "Scheduled Task"),
                args.getString("task_type", "general"),
                args.getString("target_time", ""),
                args.getString("details", "")
            );
        } else if (toolName == "save_memory") {
            autoConnect("active_memory");
            return saveMemory(
                args.getString("key", ""),
                args.getString("value", ""),
                args.getString("category", "preference")
            );
        } else if (toolName == "recall_memories") {
            autoConnect("active_memory");
            return recallMemories(args.getString("query", ""));
        } else if (toolName == "manage_action_plan") {
            autoConnect("active_memory");
            return manageActionPlan(
                args.getString("action", "list"),
                args.getString("goal", ""),
                args.getString("steps", ""),
                args.getString("status", "in_progress")
            );
        } else if (toolName == "build_and_run_custom_tool") {
            if (!envFlagEnabled("AMUSE_ALLOW_CUSTOM_TOOLS")) {
                return gatedError("AMUSE_ALLOW_CUSTOM_TOOLS", "Writing and executing custom Python/Bash tools");
            }
            autoConnect("dynamic_tools");
            return buildAndRunCustomTool(
                args.getString("tool_name", "custom_script"),
                args.getString("language", "python"),
                args.getString("code", ""),
                args.getString("description", "")
            );
        } else if (toolName == "media_and_system_control") {
            autoConnect("computer_control");
            return mediaAndSystemControl(args.getString("action", "active_window_info"));
        } else if (toolName == "browser_agent_action") {
            autoConnect("web_browser");
            return browserAgentAction(argsJson);
        } else if (toolName == "gui_desktop_action") {
            autoConnect("computer_control");
            if (!envFlagEnabled("AMUSE_ALLOW_GUI_CONTROL")) {
                return gatedError("AMUSE_ALLOW_GUI_CONTROL", "Synthetic keyboard/mouse input into desktop windows");
            }
            return guiDesktopAction(argsJson);
        }

        return "{\"error\":\"Unknown tool: " + jsonEscape(toolName) + "\"}";
    }

    void emitMemoryAndPlansToHUD() {
        std::lock_guard<std::recursive_mutex> lock(dbMutex_);
        ensureDatabase();
        std::ostringstream memBody;
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT key, value, category FROM memories ORDER BY id DESC LIMIT 15;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string k = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)) ?: "";
                std::string v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)) ?: "";
                std::string c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";
                memBody << "• [" << c << "] " << k << ": " << v << "\n";
            }
            sqlite3_finalize(stmt);
        }
        if (sqlite3_prepare_v2(db_, "SELECT goal, status, steps FROM action_plans ORDER BY updated_at DESC LIMIT 5;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string g = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)) ?: "";
                std::string s = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)) ?: "";
                std::string st = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";
                memBody << "\n📋 Plan: " << g << " (" << s << ")\n" << st << "\n";
            }
            sqlite3_finalize(stmt);
        }
        std::string memPath = getNotesDir() + "/Amuse_Active_Memory.md";
        emitRichCard({
            "note",
            "🧠 Active Memory & Action Plans",
            "Persistent Context",
            memPath,
            memBody.str().empty() ? "No memories recorded yet." : memBody.str(),
            memPath,
            "Open Memory File"
        });
    }

    void emitAllNotesToHUD() {
        std::lock_guard<std::recursive_mutex> lock(dbMutex_);
        ensureDatabase();
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT title, date, body FROM notes ORDER BY id DESC LIMIT 10;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string t = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)) ?: "";
                std::string d = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)) ?: "";
                std::string b = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";
                std::string path = writeNoteMarkdownFile(t, d, b);
                emitRichCard({
                    "note",
                    "📝 " + t,
                    "Markdown Note",
                    path,
                    b,
                    path,
                    "Open Note on Desktop"
                });
            }
            sqlite3_finalize(stmt);
        }
    }

    void emitConnectorsSummaryToHUD() {
        std::lock_guard<std::recursive_mutex> lock(dbMutex_);
        ensureDatabase();
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT id, name, category, status, permission FROM connectors ORDER BY category ASC, name ASC;";
        std::ostringstream body;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)) ?: "";
                std::string cat = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";
                std::string status = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)) ?: "";
                std::string perm = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";
                body << "• " << name << " [" << cat << "] — " << status << " (" << perm << ")\n";
            }
            sqlite3_finalize(stmt);
        }
        emitRichCard({
            "connector",
            "🔌 Amuse Connectors",
            "Auto-Connect Enabled",
            "Local + Cloud Connectors in ~/.amuse/local_data.db",
            body.str(),
            getNotesDir(),
            "Open Amuse Notes Folder"
        });
    }

private:
    sqlite3 *db_ = nullptr;
    std::recursive_mutex dbMutex_;
    std::mutex cbMutex_;
    RichCardCallback richCardCb_;
    ImageFrameCallback imageFrameCb_;
    std::string lastScreenshotPath_;
    time_t lastScreenshotTime_ = 0;

    LocalConnectorsManager() = default;
    ~LocalConnectorsManager() {
        if (db_) sqlite3_close(db_);
    }

    static std::string currentIsoTimestamp() {
        std::time_t now = std::time(nullptr);
        std::tm tm_now{};
        localtime_r(&now, &tm_now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm_now);
        std::string s(buf);
        if (s.size() >= 5) {
            s.insert(s.size() - 2, ":");
        }
        return s;
    }

    static std::string currentReadableTimestamp() {
        std::time_t now = std::time(nullptr);
        std::tm tm_now{};
        localtime_r(&now, &tm_now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
        return std::string(buf);
    }

    static std::string formatTimeInZone(const char *tzName) {
        std::string cmd = std::string("TZ=") + shellSingleQuote(tzName) + " date '+%Y-%m-%d %I:%M:%S %p %Z (%z)'";
        return runCommandCapture(cmd, 256);
    }

    static bool parseTargetTime(const std::string &targetTimeStr, int durationSec,
                                time_t &outTargetEpoch, int &outSecondsUntil, std::string &outFormattedTarget) {
        time_t now = std::time(nullptr);
        if (durationSec > 0) {
            outTargetEpoch = now + durationSec;
            outSecondsUntil = durationSec;
            std::tm tm_target{};
            localtime_r(&outTargetEpoch, &tm_target);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %I:%M:%S %p %Z", &tm_target);
            outFormattedTarget = buf;
            return true;
        }
        if (targetTimeStr.empty()) {
            outTargetEpoch = now + 60;
            outSecondsUntil = 60;
            outFormattedTarget = currentReadableTimestamp();
            return true;
        }

        // Handle Central Daylight vs Standard Time intuitively when user says "CST" during daylight saving
        std::string normalized = targetTimeStr;
        std::string cmd = "date -d " + shellSingleQuote(normalized) + " +%s 2>/dev/null";
        std::string epochStr = runCommandCapture(cmd, 128);
        if (epochStr.empty()) {
            cmd = "date -d " + shellSingleQuote("today " + normalized) + " +%s 2>/dev/null";
            epochStr = runCommandCapture(cmd, 128);
        }

        if (!epochStr.empty()) {
            long parsed = std::atol(epochStr.c_str());
            if (parsed > 0) {
                // If user gave a clock time without explicit year and it already passed today, check if they meant tomorrow
                if (parsed < now && normalized.find("202") == std::string::npos) {
                    std::string tomCmd = "date -d " + shellSingleQuote("tomorrow " + normalized) + " +%s 2>/dev/null";
                    std::string tomStr = runCommandCapture(tomCmd, 128);
                    if (!tomStr.empty() && std::atol(tomStr.c_str()) > now) {
                        parsed = std::atol(tomStr.c_str());
                    }
                }
                outTargetEpoch = static_cast<time_t>(parsed);
                outSecondsUntil = static_cast<int>(outTargetEpoch - now);
                std::tm tm_target{};
                localtime_r(&outTargetEpoch, &tm_target);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %I:%M:%S %p %Z", &tm_target);
                outFormattedTarget = buf;
                return true;
            }
        }

        outTargetEpoch = now + 60;
        outSecondsUntil = 60;
        outFormattedTarget = targetTimeStr;
        return false;
    }

    static std::string writeNoteMarkdownFile(const std::string &title, const std::string &dateStr, const std::string &body, bool overwrite = false) {
        std::string dir = getNotesDir();
        std::string filename = sanitizeFilename(title) + ".md";
        std::string fullPath = dir + "/" + filename;
        std::ostringstream desired;
        desired << "# " << title << "\n\n";
        desired << "*Updated: " << dateStr << "*\n\n";
        desired << body << "\n";
        std::string desiredStr = desired.str();
        {
            std::ifstream ifs(fullPath, std::ios::binary);
            if (ifs.is_open()) {
                std::ostringstream cur;
                cur << ifs.rdbuf();
                if (cur.str() == desiredStr) return fullPath;
                // Never clobber a file the user may have edited in their own
                // editor unless the caller explicitly overwrites (create_note).
                if (!overwrite) return fullPath;
            }
        }
        std::ofstream ofs(fullPath, std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << desiredStr;
            ofs.close();
        }
        return fullPath;
    }

    static void appendIcsEvent(const std::string &uid, const std::string &title,
                               const std::string &startIso, const std::string &location,
                               const std::string &notes) {
        std::string icsPath = getHomeDir() + "/.local/share/evolution/calendar/system/calendar.ics";
        std::ifstream ifs(icsPath);
        if (!ifs.is_open()) return;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        std::string content = ss.str();
        ifs.close();

        if (content.find(uid) != std::string::npos) return;
        size_t pos = content.rfind("END:VCALENDAR");
        if (pos == std::string::npos) return;

        std::time_t now = std::time(nullptr);
        std::tm tm_utc{};
        gmtime_r(&now, &tm_utc);
        char dtstamp[32];
        std::strftime(dtstamp, sizeof(dtstamp), "%Y%m%dT%H%M%SZ", &tm_utc);

        std::ostringstream vevent;
        vevent << "BEGIN:VEVENT\r\n"
               << "UID:" << uid << "@amuse.local\r\n"
               << "DTSTAMP:" << dtstamp << "\r\n"
               << "SUMMARY:" << title << "\r\n";
        if (!location.empty()) vevent << "LOCATION:" << location << "\r\n";
        if (!notes.empty()) vevent << "DESCRIPTION:" << notes << " (Start: " << startIso << ")\r\n";
        vevent << "END:VEVENT\r\n";

        content.insert(pos, vevent.str());
        std::ofstream ofs(icsPath, std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << content;
        }
    }

    static void appendIcsTask(const std::string &uid, const std::string &title, const std::string &notes) {
        std::string icsPath = getHomeDir() + "/.local/share/evolution/tasks/system/tasks.ics";
        std::ifstream ifs(icsPath);
        if (!ifs.is_open()) return;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        std::string content = ss.str();
        ifs.close();

        if (content.find(uid) != std::string::npos) return;
        size_t pos = content.rfind("END:VCALENDAR");
        if (pos == std::string::npos) return;

        std::time_t now = std::time(nullptr);
        std::tm tm_utc{};
        gmtime_r(&now, &tm_utc);
        char dtstamp[32];
        std::strftime(dtstamp, sizeof(dtstamp), "%Y%m%dT%H%M%SZ", &tm_utc);

        std::ostringstream vtodo;
        vtodo << "BEGIN:VTODO\r\n"
              << "UID:" << uid << "@amuse.local\r\n"
              << "DTSTAMP:" << dtstamp << "\r\n"
              << "SUMMARY:" << title << "\r\n"
              << "STATUS:NEEDS-ACTION\r\n";
        if (!notes.empty()) vtodo << "DESCRIPTION:" << notes << "\r\n";
        vtodo << "END:VTODO\r\n";

        content.insert(pos, vtodo.str());
        std::ofstream ofs(icsPath, std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << content;
        }
    }

    void ensureDatabase() {
        if (db_) return;
        std::string dir = getHomeDir() + "/.amuse";
        mkdirRecursive(dir);
        std::string dbPath = dir + "/local_data.db";

        if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
            std::cerr << "[AmuseConnectors] Failed to open SQLite DB: " << sqlite3_errmsg(db_) << std::endl;
            return;
        }

        const char *schemaSql = R"(
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                guid TEXT,
                text TEXT,
                sender TEXT,
                is_from_me INTEGER,
                date TEXT
            );
            CREATE TABLE IF NOT EXISTS contacts (
                id TEXT PRIMARY KEY,
                given_name TEXT,
                family_name TEXT,
                full_name TEXT,
                organization TEXT,
                phone_label TEXT,
                phone_number TEXT,
                email_label TEXT,
                email_address TEXT
            );
            CREATE TABLE IF NOT EXISTS calendar_events (
                id TEXT PRIMARY KEY,
                title TEXT,
                start_iso TEXT,
                end_iso TEXT,
                is_all_day INTEGER,
                location TEXT,
                notes TEXT,
                calendar TEXT
            );
            CREATE TABLE IF NOT EXISTS reminders (
                id TEXT PRIMARY KEY,
                title TEXT,
                completed INTEGER,
                due_date TEXT,
                notes TEXT,
                calendar TEXT
            );
            CREATE TABLE IF NOT EXISTS notes (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                title TEXT,
                date TEXT,
                preview TEXT,
                body TEXT
            );
            CREATE TABLE IF NOT EXISTS connectors (
                id TEXT PRIMARY KEY,
                name TEXT,
                category TEXT,
                status TEXT,
                permission TEXT,
                description TEXT,
                last_used TEXT
            );
            CREATE TABLE IF NOT EXISTS alarms (
                id TEXT PRIMARY KEY,
                label TEXT,
                target_time TEXT,
                target_epoch INTEGER,
                status TEXT,
                created_at TEXT
            );
            CREATE TABLE IF NOT EXISTS scheduled_tasks (
                id TEXT PRIMARY KEY,
                title TEXT,
                task_type TEXT,
                target_time TEXT,
                target_epoch INTEGER,
                details TEXT,
                status TEXT,
                created_at TEXT
            );
            CREATE TABLE IF NOT EXISTS social_posts (
                id TEXT PRIMARY KEY,
                platform TEXT,
                action TEXT,
                content TEXT,
                schedule_time TEXT,
                status TEXT,
                created_at TEXT
            );
            CREATE TABLE IF NOT EXISTS emails (
                id TEXT PRIMARY KEY,
                sender TEXT,
                recipient TEXT,
                subject TEXT,
                body TEXT,
                date TEXT,
                status TEXT
            );
            CREATE TABLE IF NOT EXISTS memories (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                key TEXT UNIQUE,
                value TEXT,
                category TEXT,
                updated_at TEXT
            );
            CREATE TABLE IF NOT EXISTS action_plans (
                id TEXT PRIMARY KEY,
                goal TEXT,
                steps TEXT,
                status TEXT,
                updated_at TEXT
            );
            CREATE TABLE IF NOT EXISTS custom_tools (
                name TEXT PRIMARY KEY,
                language TEXT,
                script_path TEXT,
                description TEXT,
                created_at TEXT
            );
        )";
        sqlite3_exec(db_, schemaSql, nullptr, nullptr, nullptr);

        seedIfEmpty();
        syncAllNotesToDisk();
    }

    int tableRowCount(const char *table) {
        std::string sql = std::string("SELECT COUNT(*) FROM ") + table + ";";
        sqlite3_stmt *stmt = nullptr;
        int count = 0;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        return count;
    }

    void syncAllNotesToDisk() {
        getNotesDir();
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT title, date, body FROM notes ORDER BY id ASC;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *t = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                const char *d = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                const char *b = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
                if (t) {
                    writeNoteMarkdownFile(t, d ? d : "", b ? b : "");
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    void autoConnect(const std::string &serviceId) {
        if (serviceId.empty()) return;
        std::string nowStr = currentReadableTimestamp();
        const char *sql = "UPDATE connectors SET status = 'connected', last_used = ? WHERE id = ? OR LOWER(name) LIKE ?;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, serviceId.c_str(), -1, SQLITE_TRANSIENT);
            std::string pattern = "%" + serviceId + "%";
            sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void seedIfEmpty() {
        // No fake personal data is ever seeded. Messages/contacts/calendar/reminders/notes
        // tables start empty and are filled only by real user data or real connectors.

        if (tableRowCount("connectors") < 1) {
            // Honest connector registry: local tools that genuinely work are 'connected';
            // cloud services are 'available' (advertised, not yet connected — no OAuth yet).
            const char *sql = R"(
                INSERT OR IGNORE INTO connectors (id, name, category, status, permission, description, last_used) VALUES
                ('notes', 'Local Markdown Notes (~/Documents/AmuseNotes)', 'local', 'connected', 'read_and_write', 'Reads and writes Markdown notes in ~/Documents/AmuseNotes/ and ~/Notes/', 'Active'),
                ('alarms_timers', 'System Alarms, Timers & Scheduler', 'local', 'connected', 'read_and_write', 'Background alarms, countdown timers, audio chimes, and scheduled tasks', 'Active'),
                ('active_memory', 'Active Memory & Action Plans', 'local', 'connected', 'read_and_write', 'Persists user preferences, facts, habits, and multi-step project plans across sessions', 'Active'),
                ('files_disk', 'File Assistant (home directory)', 'local', 'connected', 'read_and_write', 'Searches, reads, writes, and organizes files inside the home directory', 'Active'),
                ('computer_control', 'Screenshot & Screencast', 'local', 'connected', 'read_and_write', 'Records ffmpeg MP4 screencasts and captures PNG screenshots', 'Active'),
                ('web_browser', 'Web Search & Browser Automation', 'local', 'connected', 'read_and_write', 'Searches the web, reads webpages, and runs Playwright browser tasks', 'Active'),
                ('email', 'Email (Thunderbird & IMAP/SMTP)', 'local', 'connected', 'read_and_write', 'Searches on-device Thunderbird mail and hosted IMAP accounts; sends via SMTP with confirmation', 'Active'),
                ('messages', 'Local Messages & Chat', 'local', 'available', 'read_and_write', 'Local message store (no real message source connected yet)', 'never'),
                ('calendar', 'Local Calendar', 'local', 'available', 'read_and_write', 'Local calendar store; Evolution/EDS sync not yet wired', 'never'),
                ('reminders', 'Local Tasks', 'local', 'available', 'read_and_write', 'Local reminders store; Evolution/EDS sync not yet wired', 'never'),
                ('contacts', 'Contacts (Thunderbird address books)', 'local', 'available', 'read_and_write', 'Reads Thunderbird contact names; local store for new contacts', 'never'),
                ('gmail', 'Gmail Connector', 'cloud', 'available', 'read_only', 'Not connected: requires OAuth (planned). Generic IMAP/SMTP works today via connect_email_account.', 'never'),
                ('google_calendar', 'Google Calendar Connector', 'cloud', 'available', 'read_only', 'Not connected: requires OAuth (planned).', 'never'),
                ('google_docs', 'Google Docs & Drive Connector', 'cloud', 'available', 'read_only', 'Not connected: requires OAuth (planned).', 'never'),
                ('slack', 'Slack Workspace Connector', 'cloud', 'available', 'read_only', 'Not connected: requires OAuth (planned).', 'never'),
                ('notion', 'Notion Workspace Connector', 'cloud', 'available', 'read_only', 'Not connected: requires OAuth (planned).', 'never'),
                ('github', 'GitHub Connector', 'cloud', 'available', 'read_only', 'Not connected: requires OAuth (planned).', 'never'),
                ('whatsapp', 'WhatsApp Connector', 'cloud', 'available', 'read_only', 'Not connected: messaging channel planned.', 'never'),
                ('linkedin', 'LinkedIn Connector', 'cloud', 'available', 'read_only', 'Not connected: browser-assisted composer only (opens share URL, no API).', 'never'),
                ('spotify', 'Spotify & System Media Connector', 'cloud', 'available', 'read_only', 'Desktop media keys via MPRIS work; Spotify account API not connected.', 'never');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        // One-time cleanup of fake data seeded by older builds
        sqlite3_exec(db_,
            "DELETE FROM messages WHERE guid IN ('msg-101','msg-102','msg-103','msg-104');"
            "DELETE FROM contacts WHERE id IN ('contact-001','contact-002','contact-003','contact-004');"
            "DELETE FROM calendar_events WHERE id IN ('evt-001','evt-002');"
            "DELETE FROM reminders WHERE id IN ('rem-001','rem-002');"
            "DELETE FROM action_plans WHERE id = 'plan-001';"
            "DELETE FROM notes WHERE title IN ('Infrastructure & Servers Inventory','Amuse Agent Architecture & Roadmap','Binary Reverse Engineering Notes');"
            "DELETE FROM memories WHERE key IN ('user_name','desktop_environment','active_project') AND updated_at = '2026-10-13 05:00:00';"
            "DELETE FROM emails WHERE id IN ('mail-001', 'mail-002');",
            nullptr, nullptr, nullptr);

        // Correct the fake "connected" statuses older builds wrote for cloud connectors
        sqlite3_exec(db_,
            "UPDATE connectors SET status = 'available', last_used = 'never' "
            "WHERE category = 'cloud' AND status = 'connected' AND id IN "
            "('gmail','google_calendar','google_docs','slack','notion','github','whatsapp','linkedin','spotify');",
            nullptr, nullptr, nullptr);
    }

    std::string getCurrentTimeAndEnvironment() {
        std::time_t now = std::time(nullptr);
        std::string localReadable = currentReadableTimestamp();
        std::string localIso = currentIsoTimestamp();
        std::string pt = formatTimeInZone("America/Los_Angeles");
        std::string mt = formatTimeInZone("America/Denver");
        std::string ct = formatTimeInZone("America/Chicago");
        std::string et = formatTimeInZone("America/New_York");
        std::string utc = formatTimeInZone("UTC");

        std::ostringstream out;
        out << "{"
            << "\"local_time\":\"" << jsonEscape(localReadable) << "\","
            << "\"local_iso\":\"" << jsonEscape(localIso) << "\","
            << "\"unix_epoch\":" << now << ","
            << "\"time_in_zones\":{"
            << "\"pacific_pt\":\"" << jsonEscape(pt) << "\","
            << "\"mountain_mt\":\"" << jsonEscape(mt) << "\","
            << "\"central_cst_cdt\":\"" << jsonEscape(ct) << "\","
            << "\"eastern_et\":\"" << jsonEscape(et) << "\","
            << "\"utc\":\"" << jsonEscape(utc) << "\""
            << "},"
            << "\"os\":\"Ubuntu Linux 24.04 Desktop (X11/GNOME)\","
            << "\"notes_directory\":\"" << jsonEscape(getNotesDir()) << "\","
            << "\"notes_symlink\":\"" << jsonEscape(getHomeDir() + "/Notes") << "\","
            << "\"videos_directory\":\"" << jsonEscape(getVideosDir()) << "\","
            << "\"pictures_directory\":\"" << jsonEscape(getPicturesDir()) << "\","
            << "\"linux_notes_info\":\"On Linux, notes are stored as Markdown (.md) files in "
            << jsonEscape(getNotesDir()) << " and displayed in the Amuse HUD.\""
            << "}";
        return out.str();
    }

    // Probe the actual backend state for a connector at query time.
    // Returns "connected" only when the tool will genuinely work right now.
    static std::string probeBackend(const std::string &id) {
        if (id == "messages" || id == "calendar" || id == "reminders" || id == "contacts") {
            // Local SQLite stores; check whether Evolution Data Server is reachable
            int rc = system("busctl call org.gnome.evolution.dataserver.Calendar /org/gnome/evolution/dataserver/Calendar org.freedesktop.DBus.Peer Ping >/dev/null 2>&1");
            return rc == 0 ? "connected" : "local_stub_no_eds";
        }
        if (id == "computer_control") {
            if (access("/usr/bin/ffmpeg", X_OK) != 0) return "missing_ffmpeg";
            if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return "no_display";
            return "connected";
        }
        if (id == "web_browser") {
            int rc = system("python3 -c 'import playwright' >/dev/null 2>&1");
            return rc == 0 ? "connected" : "missing_playwright";
        }
        if (id == "alarms_timers") {
            if (access("/usr/bin/notify-send", X_OK) != 0) return "missing_notify_send";
            return "connected";
        }
        // email (self-contained IMAP/SMTP), notes, memory, files, web search: no external deps
        return "connected";
    }

    std::string listConnectors(const std::string &category) {
        const bool filter = !category.empty() && category != "all";
        std::string sql = "SELECT id, name, category, status, permission, description, last_used FROM connectors ";
        if (filter) {
            sql += "WHERE category = ? ";
        }
        sql += "ORDER BY category ASC, name ASC;";
        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"connectors\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (filter) {
                sqlite3_bind_text(stmt, 1, category.c_str(), -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"name\":\"" << jsonEscape(col(1)) << "\","
                    << "\"category\":\"" << jsonEscape(col(2)) << "\","
                    << "\"status\":\"" << jsonEscape(col(3)) << "\","
                    << "\"readiness\":\"" << jsonEscape(col(2) == "cloud" ? "available_no_oauth" : probeBackend(col(0))) << "\","
                    << "\"permission\":\"" << jsonEscape(col(4)) << "\","
                    << "\"description\":\"" << jsonEscape(col(5)) << "\","
                    << "\"last_used\":\"" << jsonEscape(col(6)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string connectService(const std::string &service, const std::string &permission, bool openInBrowser) {
        std::string nowStr = currentReadableTimestamp();
        std::string perm = permission.empty() ? "read_and_write" : permission;
        autoConnect(service);

        const char *upsertSql =
            "INSERT INTO connectors (id, name, category, status, permission, description, last_used) "
            "VALUES (?, ?, 'cloud', 'connected', ?, 'Connected external service', ?) "
            "ON CONFLICT(id) DO UPDATE SET status='connected', permission=excluded.permission, last_used=excluded.last_used;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, upsertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, service.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, service.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, perm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        std::string url;
        std::string lower = service;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("linkedin") != std::string::npos) url = "https://www.linkedin.com/feed/";
        else if (lower.find("gmail") != std::string::npos || lower.find("mail") != std::string::npos) url = "https://mail.google.com/";
        else if (lower.find("calendar") != std::string::npos) url = "https://calendar.google.com/";
        else if (lower.find("slack") != std::string::npos) url = "https://app.slack.com/";
        else if (lower.find("notion") != std::string::npos) url = "https://www.notion.so/";
        else if (lower.find("github") != std::string::npos) url = "https://github.com/";
        else if (lower.find("whatsapp") != std::string::npos) url = "https://web.whatsapp.com/";

        if (openInBrowser && !url.empty()) {
            openOnDesktop(url);
        }

        emitRichCard({
            "connector",
            "🔌 Connector Connected: " + service,
            perm,
            url.empty() ? "Local Desktop Bridge" : url,
            "Connector '" + service + "' is active with " + perm + " permissions.",
            url,
            "Open Service"
        });

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"service\":\"" << jsonEscape(service) << "\","
            << "\"status\":\"connected\","
            << "\"permission\":\"" << jsonEscape(perm) << "\","
            << "\"service_url\":\"" << jsonEscape(url) << "\""
            << "}";
        return out.str();
    }

    std::string searchMessages(const std::string &query, const std::string &sender, int limit) {
        std::string sql = "SELECT text, sender, is_from_me, date, guid FROM messages WHERE 1=1 ";
        std::vector<std::string> params;
        if (!query.empty()) {
            sql += "AND text LIKE ? COLLATE NOCASE ";
            params.push_back("%" + query + "%");
        }
        if (!sender.empty()) {
            sql += "AND sender LIKE ? COLLATE NOCASE ";
            params.push_back("%" + sender + "%");
        }
        sql += "ORDER BY date DESC LIMIT " + std::to_string(limit) + ";";

        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"messages\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < params.size(); i++) {
                sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                const char *t = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                const char *s = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                int isFromMe = sqlite3_column_int(stmt, 2);
                const char *d = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
                const char *g = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                out << "{"
                    << "\"text\":\"" << jsonEscape(t ? t : "") << "\","
                    << "\"sender\":\"" << jsonEscape(s ? s : "") << "\","
                    << "\"is_from_me\":" << (isFromMe ? "true" : "false") << ","
                    << "\"date\":\"" << jsonEscape(d ? d : "") << "\","
                    << "\"guid\":\"" << jsonEscape(g ? g : "") << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string sendMessage(const std::string &recipient, const std::string &text, const std::string &channel) {
        std::string guid = "msg-" + std::to_string(std::time(nullptr));
        std::string dateStr = currentReadableTimestamp();
        std::string senderTag = "Me -> " + recipient + " (" + channel + ")";
        const char *sql = "INSERT INTO messages (guid, text, sender, is_from_me, date) VALUES (?, ?, ?, 1, ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, guid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, senderTag.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, dateStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        sendDesktopNotification("💬 Amuse Message Sent (" + channel + ")", "To " + recipient + ": " + text);

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"guid\":\"" << jsonEscape(guid) << "\","
            << "\"recipient\":\"" << jsonEscape(recipient) << "\","
            << "\"channel\":\"" << jsonEscape(channel) << "\","
            << "\"text\":\"" << jsonEscape(text) << "\""
            << "}";
        return out.str();
    }

    std::string searchContacts(const std::string &name, const std::string &phoneOrEmail) {
        std::string sql =
            "SELECT id, given_name, family_name, full_name, organization, phone_label, phone_number, email_label, email_address "
            "FROM contacts WHERE 1=1 ";
        std::vector<std::string> params;
        if (!name.empty()) {
            sql += "AND (full_name LIKE ? COLLATE NOCASE OR organization LIKE ? COLLATE NOCASE) ";
            params.push_back("%" + name + "%");
            params.push_back("%" + name + "%");
        }
        if (!phoneOrEmail.empty()) {
            sql += "AND (phone_number LIKE ? COLLATE NOCASE OR email_address LIKE ? COLLATE NOCASE) ";
            params.push_back("%" + phoneOrEmail + "%");
            params.push_back("%" + phoneOrEmail + "%");
        }
        sql += "LIMIT 30;";

        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"contacts\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < params.size(); i++) {
                sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"given_name\":\"" << jsonEscape(col(1)) << "\","
                    << "\"family_name\":\"" << jsonEscape(col(2)) << "\","
                    << "\"full_name\":\"" << jsonEscape(col(3)) << "\","
                    << "\"organization\":\"" << jsonEscape(col(4)) << "\","
                    << "\"phone_numbers\":[{\"label\":\"" << jsonEscape(col(5)) << "\",\"number\":\"" << jsonEscape(col(6)) << "\"}],"
                    << "\"emails\":[{\"label\":\"" << jsonEscape(col(7)) << "\",\"email\":\"" << jsonEscape(col(8)) << "\"}]"
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        std::string tbQuery = !name.empty() ? name : phoneOrEmail;
        std::string scriptPath = getRepoRootDir() + "/scripts/email_connector.py";
        std::string tbJsonPayload = "{\"action\":\"contacts\",\"query\":\"" + jsonEscape(tbQuery) + "\",\"limit\":25}";
        std::string tbCmd = "/usr/bin/python3 " + shellSingleQuote(scriptPath) + " " + shellSingleQuote(tbJsonPayload) + " 2>/dev/null";
        std::string tbOut = runCommandCapture(tbCmd, 16384);
        if (tbOut.empty() || tbOut[0] != '{') tbOut = "{}";

        out << "],\"thunderbird_on_device_contacts\":" << tbOut << "}";
        return out.str();
    }

    std::string createContact(const std::string &fullName, const std::string &phone,
                              const std::string &email, const std::string &org) {
        std::string id = "contact-" + std::to_string(std::time(nullptr));
        std::string given = fullName;
        std::string family;
        size_t sp = fullName.find(' ');
        if (sp != std::string::npos) {
            given = fullName.substr(0, sp);
            family = fullName.substr(sp + 1);
        }
        const char *sql =
            "INSERT INTO contacts (id, given_name, family_name, full_name, organization, phone_label, phone_number, email_label, email_address) "
            "VALUES (?, ?, ?, ?, ?, 'Mobile', ?, 'Work', ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, given.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, family.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, fullName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, org.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, phone.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return "{\"success\":true,\"contact_id\":\"" + jsonEscape(id) + "\",\"full_name\":\"" + jsonEscape(fullName) + "\"}";
    }

    std::string getCalendarEvents(int daysAhead) {
        (void)daysAhead;
        std::string sql =
            "SELECT id, title, start_iso, end_iso, is_all_day, location, notes, calendar "
            "FROM calendar_events ORDER BY start_iso ASC LIMIT 30;";
        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"events\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                int allDay = sqlite3_column_int(stmt, 4);
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"title\":\"" << jsonEscape(col(1)) << "\","
                    << "\"start\":\"" << jsonEscape(col(2)) << "\","
                    << "\"end\":\"" << jsonEscape(col(3)) << "\","
                    << "\"is_all_day\":" << (allDay ? "true" : "false") << ","
                    << "\"location\":\"" << jsonEscape(col(5)) << "\","
                    << "\"notes\":\"" << jsonEscape(col(6)) << "\","
                    << "\"calendar\":\"" << jsonEscape(col(7)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string createCalendarEvent(const std::string &title, const std::string &startIso,
                                    const std::string &endIso, const std::string &location,
                                    const std::string &notes) {
        std::string id = "evt-" + std::to_string(std::time(nullptr));
        std::string actualEnd = endIso.empty() ? startIso : endIso;
        const char *sql =
            "INSERT INTO calendar_events (id, title, start_iso, end_iso, is_all_day, location, notes, calendar) "
            "VALUES (?, ?, ?, ?, 0, ?, ?, 'Personal');";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, startIso.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, actualEnd.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, location.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        appendIcsEvent(id, title, startIso, location, notes);
        sendDesktopNotification("📅 Calendar Event Created", title + " @ " + startIso);

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"event_id\":\"" << jsonEscape(id) << "\","
            << "\"title\":\"" << jsonEscape(title) << "\","
            << "\"start\":\"" << jsonEscape(startIso) << "\","
            << "\"synced_to\":\"~/.local/share/evolution/calendar/system/calendar.ics\""
            << "}";
        return out.str();
    }

    std::string getReminders(bool includeCompleted) {
        std::string sql =
            "SELECT id, title, completed, due_date, notes, calendar FROM reminders ";
        if (!includeCompleted) {
            sql += "WHERE completed = 0 ";
        }
        sql += "ORDER BY due_date ASC LIMIT 30;";
        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"reminders\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                int completed = sqlite3_column_int(stmt, 2);
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"title\":\"" << jsonEscape(col(1)) << "\","
                    << "\"completed\":" << (completed ? "true" : "false") << ","
                    << "\"due_date\":\"" << jsonEscape(col(3)) << "\","
                    << "\"notes\":\"" << jsonEscape(col(4)) << "\","
                    << "\"calendar\":\"" << jsonEscape(col(5)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string createReminder(const std::string &title, const std::string &dueIso, const std::string &notes) {
        std::string id = "rem-" + std::to_string(std::time(nullptr));
        const char *sql =
            "INSERT INTO reminders (id, title, completed, due_date, notes, calendar) "
            "VALUES (?, ?, 0, ?, ?, 'Tasks');";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, dueIso.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        appendIcsTask(id, title, notes);
        sendDesktopNotification("✅ Reminder Created", title + (dueIso.empty() ? "" : (" (Due: " + dueIso + ")")));

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"reminder_id\":\"" << jsonEscape(id) << "\","
            << "\"title\":\"" << jsonEscape(title) << "\","
            << "\"due_date\":\"" << jsonEscape(dueIso) << "\","
            << "\"synced_to\":\"~/.local/share/evolution/tasks/system/tasks.ics\""
            << "}";
        return out.str();
    }

    std::string setAlarmOrTimer(const std::string &label, const std::string &targetTimeStr, int durationSec) {
        time_t targetEpoch = 0;
        int secondsUntil = 0;
        std::string formattedTarget;
        if (!parseTargetTime(targetTimeStr, durationSec, targetEpoch, secondsUntil, formattedTarget)) {
            return std::string("{\"error\":\"unparseable_time\",\"message\":\"Could not understand the time '\") +
                   jsonEscape(targetTimeStr) + "'. Give a duration (e.g. duration_seconds: 900) or a clear time (e.g. '7:30 PM', 'in 20 minutes'). No alarm was set.\"}";
        }
        if (secondsUntil < 1) secondsUntil = 1;

        std::string id = "alarm-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(rand() % 1000);
        std::string nowStr = currentReadableTimestamp();

        const char *sql =
            "INSERT INTO alarms (id, label, target_time, target_epoch, status, created_at) "
            "VALUES (?, ?, ?, ?, 'armed', ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, label.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, formattedTarget.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(targetEpoch));
            sqlite3_bind_text(stmt, 5, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        sendDesktopNotification("⏰ Amuse Alarm Armed", label + " — set for " + formattedTarget);
        emitRichCard({
            "alarm",
            "⏰ Alarm Armed: " + label,
            "ARMED",
            "Target: " + formattedTarget + " (in " + std::to_string(secondsUntil) + "s)",
            "System audio chime and critical desktop notification will trigger automatically.",
            "",
            ""
        });

        // Arm background timer thread
        std::thread([this, id, label, formattedTarget, secondsUntil]() {
            int waitSec = std::min(secondsUntil, 86400);
            std::this_thread::sleep_for(std::chrono::seconds(waitSec));
            {
                std::lock_guard<std::recursive_mutex> lk(dbMutex_);
                if (db_) {
                    std::string upd = "UPDATE alarms SET status='triggered' WHERE id='" + jsonEscape(id) + "';";
                    sqlite3_exec(db_, upd.c_str(), nullptr, nullptr, nullptr);
                }
            }
            sendDesktopNotification("⏰ ALARM RINGING: " + label, "Scheduled for " + formattedTarget, true);
            LinuxAudioEngine::instance().playAlarmChime();
            emitRichCard({
                "alarm",
                "🔔 ALARM RINGING: " + label,
                "RINGING",
                formattedTarget,
                "Your scheduled alarm/timer '" + label + "' is firing now!",
                "",
                ""
            });
        }).detach();

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"alarm_id\":\"" << jsonEscape(id) << "\","
            << "\"label\":\"" << jsonEscape(label) << "\","
            << "\"target_time\":\"" << jsonEscape(formattedTarget) << "\","
            << "\"seconds_until_trigger\":" << secondsUntil << ","
            << "\"status\":\"armed\""
            << "}";
        return out.str();
    }

    std::string listAlarmsAndTimers() {
        std::ostringstream out;
        out << "{\"alarms\":[";
        bool first = true;
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id, label, target_time, target_epoch, status FROM alarms ORDER BY target_epoch ASC LIMIT 25;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"label\":\"" << jsonEscape(col(1)) << "\","
                    << "\"target_time\":\"" << jsonEscape(col(2)) << "\","
                    << "\"target_epoch\":" << sqlite3_column_int64(stmt, 3) << ","
                    << "\"status\":\"" << jsonEscape(col(4)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "],\"scheduled_tasks\":[";
        first = true;
        if (sqlite3_prepare_v2(db_, "SELECT id, title, task_type, target_time, details, status FROM scheduled_tasks ORDER BY target_epoch ASC LIMIT 25;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"title\":\"" << jsonEscape(col(1)) << "\","
                    << "\"task_type\":\"" << jsonEscape(col(2)) << "\","
                    << "\"target_time\":\"" << jsonEscape(col(3)) << "\","
                    << "\"details\":\"" << jsonEscape(col(4)) << "\","
                    << "\"status\":\"" << jsonEscape(col(5)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string searchNotes(const std::string &query, int limit) {
        std::string sql = "SELECT title, date, preview, body FROM notes ";
        std::vector<std::string> params;
        if (!query.empty()) {
            sql += "WHERE (title LIKE ? COLLATE NOCASE OR body LIKE ? COLLATE NOCASE OR preview LIKE ? COLLATE NOCASE) ";
            params.push_back("%" + query + "%");
            params.push_back("%" + query + "%");
            params.push_back("%" + query + "%");
        }
        sql += "ORDER BY id DESC LIMIT " + std::to_string(limit) + ";";

        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"notes_directory\":\"" << jsonEscape(getNotesDir()) << "\",\"notes\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < params.size(); i++) {
                sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                std::string title = col(0);
                std::string dateStr = col(1);
                std::string body = col(3);
                std::string filePath = writeNoteMarkdownFile(title, dateStr, body);
                out << "{"
                    << "\"title\":\"" << jsonEscape(title) << "\","
                    << "\"date\":\"" << jsonEscape(dateStr) << "\","
                    << "\"preview\":\"" << jsonEscape(col(2)) << "\","
                    << "\"body\":\"" << jsonEscape(body) << "\","
                    << "\"file_path\":\"" << jsonEscape(filePath) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string createNote(const std::string &title, const std::string &body, bool openDesktop) {
        std::string dateStr = currentReadableTimestamp();
        std::string preview = body;
        size_t nl = preview.find('\n');
        if (nl != std::string::npos) preview = preview.substr(0, nl);
        if (preview.size() > 160) preview = preview.substr(0, 160);

        const char *sql = "INSERT INTO notes (title, date, preview, body) VALUES (?, ?, ?, ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, dateStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, preview.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, body.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        std::string filePath = writeNoteMarkdownFile(title, dateStr, body, /*overwrite=*/true);
        sendDesktopNotification("📝 Amuse Note Saved", "Saved to " + filePath);

        emitRichCard({
            "note",
            "📝 " + title,
            "Saved to Disk",
            filePath,
            body,
            filePath,
            "Open Note on Desktop"
        });

        if (openDesktop) {
            openOnDesktop(filePath);
        }

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"title\":\"" << jsonEscape(title) << "\","
            << "\"file_path\":\"" << jsonEscape(filePath) << "\","
            << "\"notes_directory\":\"" << jsonEscape(getNotesDir()) << "\","
            << "\"displayed_in_hud\":true,"
            << "\"linux_note_hint\":\"Saved as Markdown file at " << jsonEscape(filePath)
            << " and displayed as a Note Card right inside the Amuse HUD. (Linux has no Apple Notes app.)\""
            << "}";
        return out.str();
    }

    std::string openNote(const std::string &query, bool openDesktop) {
        std::string sql = "SELECT title, date, body FROM notes ";
        if (!query.empty()) {
            sql += "WHERE (title LIKE ? COLLATE NOCASE OR body LIKE ? COLLATE NOCASE) ";
        }
        sql += "ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt *stmt = nullptr;
        std::string title, dateStr, body;
        bool found = false;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (!query.empty()) {
                std::string p = "%" + query + "%";
                sqlite3_bind_text(stmt, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, p.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                found = true;
                title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)) ?: "";
                dateStr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)) ?: "";
                body = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";
            }
            sqlite3_finalize(stmt);
        }

        if (!found) {
            return "{\"error\":\"No matching note found in ~/Documents/AmuseNotes/\"}";
        }

        std::string filePath = writeNoteMarkdownFile(title, dateStr, body);
        emitRichCard({
            "note",
            "📝 " + title,
            "Opened",
            filePath,
            body,
            filePath,
            "Open Note on Desktop"
        });
        if (openDesktop) {
            openOnDesktop(filePath);
        }

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"title\":\"" << jsonEscape(title) << "\","
            << "\"date\":\"" << jsonEscape(dateStr) << "\","
            << "\"body\":\"" << jsonEscape(body) << "\","
            << "\"file_path\":\"" << jsonEscape(filePath) << "\","
            << "\"opened_on_desktop\":" << (openDesktop ? "true" : "false")
            << "}";
        return out.str();
    }

    std::string socialMediaAction(const std::string &platform, const std::string &action,
                                  const std::string &content, const std::string &scheduleTime,
                                  bool openInBrowser, bool captureScreenshot = false,
                                  const std::string &imagePath = "", bool autoSubmit = false) {
        std::string id = "post-" + std::to_string(std::time(nullptr));
        std::string nowStr = currentReadableTimestamp();
        std::string status = scheduleTime.empty() ? "published_to_composer" : "scheduled";

        // Determine if a screenshot should be attached
        std::string attachedImage;
        time_t nowEpoch = std::time(nullptr);
        if (!imagePath.empty() && imagePath != "latest") {
            std::string expanded = expandTilde(imagePath);
            struct stat st{};
            if (stat(expanded.c_str(), &st) == 0 && st.st_size > 0) {
                attachedImage = expanded;
            }
        }
        if (attachedImage.empty() && !lastScreenshotPath_.empty() && (nowEpoch - lastScreenshotTime_) <= 120) {
            struct stat st{};
            if (stat(lastScreenshotPath_.c_str(), &st) == 0 && st.st_size > 0) {
                attachedImage = lastScreenshotPath_;
            }
        }
        if (attachedImage.empty() && (captureScreenshot || imagePath == "latest")) {
            takeScreenshot("Social_Post_Screenshot", false);
            attachedImage = lastScreenshotPath_;
        }

        const char *sql =
            "INSERT INTO social_posts (id, platform, action, content, schedule_time, status, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, platform.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, action.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, scheduleTime.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Save as Markdown note draft as well
        std::string draftPath = writeNoteMarkdownFile("LinkedIn_Post_Draft", nowStr, content, true);
        if (!attachedImage.empty()) {
            copyImageToClipboard(attachedImage);
        } else {
            copyToClipboard(content);
        }

        std::string lowerPlat = platform;
        std::transform(lowerPlat.begin(), lowerPlat.end(), lowerPlat.begin(), ::tolower);
        std::string shareUrl = "https://www.linkedin.com/feed/?shareActive=true&text=" + urlEncode(content);
        if (lowerPlat.find("twitter") != std::string::npos || lowerPlat == "x" || lowerPlat == "twitter_x") {
            shareUrl = "https://twitter.com/intent/tweet?text=" + urlEncode(content);
        } else if (lowerPlat.find("reddit") != std::string::npos) {
            shareUrl = "https://www.reddit.com/submit?selftext=true&title=" + urlEncode(content.substr(0, 120)) +
                       "&text=" + urlEncode(content);
        } else if (lowerPlat.find("hackernews") != std::string::npos || lowerPlat == "hn") {
            shareUrl = "https://news.ycombinator.com/submitlink?t=" + urlEncode(content.substr(0, 80));
        }

        std::string scheduledTarget;
        int secondsUntil = 0;
        if (!scheduleTime.empty()) {
            time_t targetEpoch = 0;
            if (!parseTargetTime(scheduleTime, 0, targetEpoch, secondsUntil, scheduledTarget)) {
                return std::string("{\"error\":\"unparseable_time\",\"message\":\"Could not understand schedule_time '\") +
                       jsonEscape(scheduleTime) + "'. Nothing was scheduled.\"}";
            }
            setAlarmOrTimer("Publish " + platform + " Post", scheduleTime, 0);
            createCalendarEvent("Publish " + platform + " Post", scheduledTarget, "", shareUrl, content);
        }

        bool launchedBrowser = false;
        if (scheduleTime.empty() && (openInBrowser || action == "post_now" || action == "open_composer")) {
            openOnDesktop(shareUrl);
            launchedBrowser = true;

            if (!attachedImage.empty() || autoSubmit) {
                std::thread([attachedImage, autoSubmit, platform]() {
                    // Wait for browser tab & LinkedIn/Twitter composer modal to finish opening and focusing
                    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
                    if (!attachedImage.empty()) {
                        copyImageToClipboard(attachedImage);
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        x11SendKeyCombo("Ctrl+v");
                        sendDesktopNotification(
                            "📸 Screenshot Pasted into " + platform,
                            "Attached " + attachedImage + " via X11 clipboard + Ctrl+V"
                        );
                    }
                    if (autoSubmit) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                        x11SendKeyCombo("Ctrl+Return");
                    }
                }).detach();
            }
        }

        sendDesktopNotification(
            "🚀 " + platform + " Post " + (scheduleTime.empty() ? "Launched in Browser" : "Scheduled"),
            !attachedImage.empty()
                ? ("Text pre-filled & pasting screenshot: " + attachedImage)
                : (scheduleTime.empty() ? "Opened live composer & copied to clipboard" : ("Scheduled for " + scheduledTarget))
        );

        emitRichCard({
            "connector",
            "🚀 " + platform + " Post (" + status + ")",
            platform,
            !attachedImage.empty()
                ? ("Screenshot Attached: " + attachedImage)
                : (scheduleTime.empty() ? ("Saved to " + draftPath) : ("Scheduled for " + scheduledTarget)),
            content + (!attachedImage.empty() ? ("\n\n📸 Attached Screenshot: " + attachedImage) : ""),
            shareUrl,
            "Open " + platform + " Composer"
        });

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"post_id\":\"" << jsonEscape(id) << "\","
            << "\"platform\":\"" << jsonEscape(platform) << "\","
            << "\"status\":\"" << jsonEscape(status) << "\","
            << "\"launched_browser_composer\":" << (launchedBrowser ? "true" : "false") << ","
            << "\"attached_screenshot_path\":\"" << jsonEscape(attachedImage) << "\","
            << "\"auto_paste_screenshot_via_x11\":" << (!attachedImage.empty() ? "true" : "false") << ","
            << "\"auto_submit\":" << (autoSubmit ? "true" : "false") << ","
            << "\"scheduled_for\":\"" << jsonEscape(scheduledTarget) << "\","
            << "\"seconds_until_scheduled\":" << secondsUntil << ","
            << "\"copied_to_clipboard\":true,"
            << "\"draft_file_path\":\"" << jsonEscape(draftPath) << "\","
            << "\"composer_url\":\"" << jsonEscape(shareUrl) << "\""
            << "}";
        return out.str();
    }

    std::string emailAction(const std::string &argsJson) {
        JsonHelper args(argsJson);
        std::string action = args.getString("action", "search");
        std::string query = args.getString("query", "");
        std::string to = args.getString("to", "");
        std::string subject = args.getString("subject", "");
        std::string body = args.getString("body", "");
        std::string attachmentPath = args.getString("attachment_path", "");

        if (action == "send") {
            // Sending real email requires explicit per-call confirmation
            if (!args.getBool("confirm_send", false)) {
                std::ostringstream preview;
                preview << "{"
                        << "\"success\":false,"
                        << "\"status\":\"needs_confirmation\","
                        << "\"message\":\"This email has NOT been sent. Show the user this exact draft and ask for explicit confirmation. Then call email_action again with the same arguments plus confirm_send: true.\","
                        << "\"draft\":{"
                        << "\"to\":\"" << jsonEscape(to) << "\","
                        << "\"subject\":\"" << jsonEscape(subject) << "\","
                        << "\"body\":\"" << jsonEscape(body) << "\","
                        << "\"attachment_path\":\"" << jsonEscape(attachmentPath) << "\""
                        << "}}";
                return preview.str();
            }
            // Attachments must live in the user's home and not in sensitive dirs
            if (!attachmentPath.empty() && !isPathAllowedForSharing(attachmentPath)) {
                return "{\"success\":false,\"error\":\"attachment_blocked\",\"message\":\"Attachment path is outside the allowed area (must be inside the user's home directory and not in a sensitive location like ~/.ssh, ~/.gnupg, ~/.aws, ~/.amuse or ~/Library).\"}";
            }
        }

        std::string scriptPath = getRepoRootDir() + "/scripts/email_connector.py";
        std::string outJson = runProcessWithIO({"/usr/bin/python3", scriptPath, "-"},
                                               argsJson.empty() ? "{\"action\":\"search\"}" : argsJson,
                                               49152, 45);
        while (!outJson.empty() && (outJson.back() == '\n' || outJson.back() == '\r')) {
            outJson.pop_back();
        }
        if (outJson.empty() || outJson[0] != '{') {
            outJson = "{\"success\":false,\"error\":\"Email connector script returned invalid output\"}";
        }

        if (action == "search" || action == "read_latest" || action == "list_accounts") {
            std::string searchLabel = !query.empty() ? query : (!subject.empty() ? subject : "Latest Inbox Messages");
            emitRichCard({
                "connector",
                "✉️ Email Connector (Thunderbird + IMAP)",
                action,
                "On-Device Thunderbird database + connected IMAP accounts",
                "Queried real emails for: " + searchLabel + "\n(Never hallucinated — read directly from Thunderbird global-messages-db.sqlite & connected IMAP accounts.)",
                "https://mail.google.com/",
                "Open Mail in Browser"
            });
            return outJson;
        }

        // For send / draft / open_composer, also record in local_data.db emails log and show rich card
        std::string id = "mail-" + std::to_string(std::time(nullptr));
        std::string nowStr = currentReadableTimestamp();
        const char *sql =
            "INSERT INTO emails (id, sender, recipient, subject, body, date, status) "
            "VALUES (?, 'me', ?, ?, ?, ?, ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, to.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, subject.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, body.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, action.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        std::string gmailUrl = "https://mail.google.com/mail/?view=cm&fs=1&to=" + urlEncode(to) +
                               "&su=" + urlEncode(subject) + "&body=" + urlEncode(body);
        sendDesktopNotification("✉️ Email " + action, "To: " + to + " | Subject: " + subject);
        emitRichCard({
            "connector",
            "✉️ Email: " + (subject.empty() ? "(No Subject)" : subject),
            action,
            "To: " + to + (attachmentPath.empty() ? "" : (" | Attachment: " + attachmentPath)),
            body,
            gmailUrl,
            "Open in Gmail"
        });

        return outJson;
    }

    std::string connectEmailAccount(const std::string &argsJson) {
        JsonHelper args(argsJson);
        std::string emailAddr = args.getString("email", "");
        std::string password = args.getString("password", "");
        std::string provider = args.getString("provider", "");
        std::string imapHost = args.getString("imap_host", "");
        std::string smtpHost = args.getString("smtp_host", "");
        int imapPort = args.getInt("imap_port", 993);
        int smtpPort = args.getInt("smtp_port", 465);

        std::ostringstream payload;
        payload << "{"
                << "\"action\":\"connect_account\","
                << "\"email\":\"" << jsonEscape(emailAddr) << "\","
                << "\"password\":\"" << jsonEscape(password) << "\","
                << "\"provider\":\"" << jsonEscape(provider) << "\","
                << "\"imap_host\":\"" << jsonEscape(imapHost) << "\","
                << "\"smtp_host\":\"" << jsonEscape(smtpHost) << "\","
                << "\"imap_port\":" << imapPort << ","
                << "\"smtp_port\":" << smtpPort
                << "}";

        std::string scriptPath = getRepoRootDir() + "/scripts/email_connector.py";
        std::string outJson = runProcessWithIO({"/usr/bin/python3", scriptPath, "-"},
                                               payload.str(), 16384, 30);
        while (!outJson.empty() && (outJson.back() == '\n' || outJson.back() == '\r')) {
            outJson.pop_back();
        }
        if (outJson.empty() || outJson[0] != '{') {
            outJson = "{\"success\":false,\"error\":\"Failed to run email connector\"}";
        }

        bool ok = outJson.find("\"success\": true") != std::string::npos ||
                  outJson.find("\"success\":true") != std::string::npos;
        sendDesktopNotification(
            ok ? "✉️ Hosted Email Connected" : "⚠️ Hosted Email Connection Needs Attention",
            emailAddr.empty() ? "Configured hosted IMAP/SMTP account" : emailAddr
        );
        emitRichCard({
            "connector",
            (ok ? "✉️ Hosted Email Connected: " : "⚠️ Hosted Email Setup: ") + emailAddr,
            ok ? "CONNECTED" : "CHECK CREDENTIALS",
            "Saved to ~/.amuse/email_accounts.json",
            outJson,
            "https://mail.google.com/",
            "Open Mail"
        });
        return outJson;
    }

    std::string recordScreencast(const std::string &title, int durationSec, const std::string &scheduleTime) {
        if (durationSec <= 0) durationSec = 5;
        if (durationSec > 60) durationSec = 60;

        if (!scheduleTime.empty()) {
            return scheduleTask("Record Screencast: " + title, "screencast", scheduleTime,
                                "duration=" + std::to_string(durationSec));
        }

        std::string outDir = getVideosDir();
        std::string filePath = outDir + "/" + sanitizeFilename(title) + "_" + std::to_string(std::time(nullptr)) + ".mp4";
        const char *dpy = getenv("DISPLAY");
        std::string displayStr = (dpy && *dpy) ? dpy : ":1";

        sendDesktopNotification("🎬 Recording Screencast", "Recording desktop for " + std::to_string(durationSec) + "s...");

        std::string cmd = "ffmpeg -y -f x11grab -framerate 15 -i " + shellSingleQuote(displayStr) +
                          " -t " + std::to_string(durationSec) +
                          " -c:v libx264 -preset ultrafast -pix_fmt yuv420p " +
                          shellSingleQuote(filePath) + " -loglevel error 2>&1";
        runCommandCapture(cmd, 1024);

        struct stat st{};
        bool ok = (stat(filePath.c_str(), &st) == 0 && st.st_size > 0);
        if (ok) {
            sendDesktopNotification("🎬 Screencast Saved", "Saved to " + filePath);
            emitRichCard({
                "media",
                "🎬 Screencast Recorded: " + title,
                std::to_string(durationSec) + "s MP4",
                filePath,
                "Recorded live desktop video (" + std::to_string(st.st_size / 1024) + " KB).",
                filePath,
                "Play Screencast"
            });
        }

        std::ostringstream out;
        out << "{"
            << "\"success\":" << (ok ? "true" : "false") << ","
            << "\"file_path\":\"" << jsonEscape(filePath) << "\","
            << "\"duration_seconds\":" << durationSec << ","
            << "\"size_bytes\":" << (ok ? st.st_size : 0)
            << "}";
        return out.str();
    }

    std::string takeScreenshot(const std::string &title, bool openAfter) {
        std::string outDir = getPicturesDir();
        std::string filePath = outDir + "/" + sanitizeFilename(title) + "_" + std::to_string(std::time(nullptr)) + ".png";
        const char *dpy = getenv("DISPLAY");
        std::string displayStr = (dpy && *dpy) ? dpy : ":1";

        std::string cmd = "ffmpeg -y -f x11grab -i " + shellSingleQuote(displayStr) +
                          " -vf scale=1280:-1 -frames:v 1 " + shellSingleQuote(filePath) + " -loglevel error 2>&1";
        runCommandCapture(cmd, 1024);

        struct stat st{};
        bool ok = (stat(filePath.c_str(), &st) == 0 && st.st_size > 0);
        if (ok) {
            lastScreenshotPath_ = filePath;
            lastScreenshotTime_ = std::time(nullptr);
            copyImageToClipboard(filePath);

            // Stream PNG into Gemini Multimodal Live Vision 400ms AFTER returning tool_call_response
            // so realtimeInput.mediaChunks never interrupts multi-tool chains (e.g. take_screenshot -> social_media_action)
            std::thread([this, filePath]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                std::ifstream ifs(filePath, std::ios::binary);
                if (ifs.is_open()) {
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    if (!bytes.empty()) {
                        emitImageFrame(base64Encode(bytes.data(), bytes.size()), "image/png");
                    }
                }
            }).detach();

            sendDesktopNotification("📸 Screenshot Captured & Copied", "Saved to " + filePath + " & copied to clipboard");
            emitRichCard({
                "media",
                "📸 Screen Vision Shared: " + title,
                "LIVE VISION",
                filePath,
                "Captured desktop screen (" + std::to_string(st.st_size / 1024) + " KB), copied image to X11 clipboard, and streamed to Gemini Multimodal Live.",
                filePath,
                "Open Screenshot"
            });
            if (openAfter) openOnDesktop(filePath);
        }

        std::string activeWin = runCommandCapture("xprop -root _NET_ACTIVE_WINDOW 2>/dev/null | awk '{print $NF}' | xargs -I{} xprop -id {} WM_NAME 2>/dev/null", 512);

        std::ostringstream out;
        out << "{"
            << "\"success\":" << (ok ? "true" : "false") << ","
            << "\"file_path\":\"" << jsonEscape(filePath) << "\","
            << "\"copied_image_to_clipboard\":" << (ok ? "true" : "false") << ","
            << "\"streamed_to_gemini_vision\":" << (ok && hasImageFrameSink() ? "true" : "false") << ","
            << "\"active_window\":\"" << jsonEscape(activeWin) << "\","
            << "\"size_bytes\":" << (ok ? st.st_size : 0)
            << "}";
        return out.str();
    }

    static std::string expandTilde(const std::string &p) {
        if (p.empty()) return getHomeDir() + "/Documents";
        if (p[0] == '~') return getHomeDir() + p.substr(1);
        return p;
    }

    std::string manageFiles(const std::string &action, const std::string &rawPath, const std::string &query) {
        std::string path = expandTilde(rawPath);

        // Path guard: operate only inside the user's home (or /tmp), never in
        // sensitive locations. Prevents reading ~/.ssh or overwriting ~/.bashrc.
        auto pathAllowed = [&](const std::string &p, bool forWrite) -> std::string {
            char resolved[PATH_MAX];
            std::string rp;
            if (realpath(p.c_str(), resolved)) {
                rp = resolved;
            } else {
                rp = p; // may not exist yet (write of new file)
            }
            std::string home = getHomeDir();
            bool inHome = rp.rfind(home + "/", 0) == 0;
            bool inTmp = rp.rfind("/tmp/", 0) == 0;
            if (!inHome && !inTmp) return "path must be inside the user's home directory or /tmp";
            static const char *deny[] = {
                "/.ssh", "/.gnupg", "/.aws", "/.config/gcloud", "/.amuse/custom_tools",
                "/.kube", "/.docker", "/.local/share/keyrings", "/Library"
            };
            std::string rel = inHome ? rp.substr(home.size()) : "";
            for (const char *d : deny) {
                if (!rel.empty() && (rel == d || rel.rfind(std::string(d) + "/", 0) == 0)) {
                    return std::string("path is in a protected location (") + d + ")";
                }
            }
            if (forWrite && inHome) {
                // Never overwrite dotfiles directly under $HOME (.bashrc etc.)
                std::string base = rp.substr(home.size() + 1);
                if (base.find('/') == std::string::npos && !base.empty() && base[0] == '.') {
                    return "refusing to modify a dotfile directly under the home directory";
                }
            }
            return "";
        };
        bool isWrite = (action == "write" || action == "move" || action == "trash");
        std::string guard = pathAllowed(path, isWrite);
        if (!guard.empty()) {
            return "{\"error\":\"path_blocked\",\"message\":\"" + jsonEscape(guard) + "\"}";
        }

        if (action == "list") {
            DIR *dir = opendir(path.c_str());
            if (!dir) return "{\"error\":\"Cannot open directory: " + jsonEscape(path) + "\"}";
            std::ostringstream out;
            out << "{\"directory\":\"" << jsonEscape(path) << "\",\"entries\":[";
            bool first = true;
            struct dirent *ent;
            int count = 0;
            while ((ent = readdir(dir)) != nullptr && count < 60) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                if (!first) out << ",";
                first = false;
                count++;
                out << "\"" << jsonEscape(ent->d_name) << "\"";
            }
            closedir(dir);
            out << "]}";
            return out.str();
        } else if (action == "search") {
            std::string cmd = "find " + shellSingleQuote(path) + " -maxdepth 3 -iname " +
                              shellSingleQuote("*" + query + "*") + " 2>/dev/null | head -n 30";
            std::string res = runCommandCapture(cmd, 8192);
            return "{\"directory\":\"" + jsonEscape(path) + "\",\"matches\":\"" + jsonEscape(res) + "\"}";
        } else if (action == "read") {
            std::ifstream ifs(path);
            if (!ifs.is_open()) return "{\"error\":\"Cannot read file: " + jsonEscape(path) + "\"}";
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.size() > 8000) content = content.substr(0, 8000) + "\n...[truncated]";
            return "{\"path\":\"" + jsonEscape(path) + "\",\"content\":\"" + jsonEscape(content) + "\"}";
        } else if (action == "write") {
            std::ofstream ofs(path, std::ios::out | std::ios::trunc);
            if (!ofs.is_open()) return "{\"error\":\"Cannot write file: " + jsonEscape(path) + "\"}";
            ofs << query;
            ofs.close();
            return "{\"success\":true,\"path\":\"" + jsonEscape(path) + "\"}";
        } else if (action == "move") {
            std::string dst = expandTilde(query);
            int rc = rename(path.c_str(), dst.c_str());
            return rc == 0 ? "{\"success\":true,\"from\":\"" + jsonEscape(path) + "\",\"to\":\"" + jsonEscape(dst) + "\"}"
                           : "{\"error\":\"Move failed\"}";
        } else if (action == "trash") {
            std::string trashDir = getHomeDir() + "/.local/share/Trash/files";
            mkdirRecursive(trashDir);
            size_t slash = path.rfind('/');
            std::string base = (slash != std::string::npos) ? path.substr(slash + 1) : path;
            std::string dst = trashDir + "/" + base;
            int rc = rename(path.c_str(), dst.c_str());
            return rc == 0 ? "{\"success\":true,\"moved_to_trash\":\"" + jsonEscape(dst) + "\"}"
                           : "{\"error\":\"Failed to move file to Trash\"}";
        }

        return "{\"error\":\"Unsupported manage_files action: " + jsonEscape(action) + "\"}";
    }

    std::string computerControlAction(const std::string &action, const std::string &target) {
        if (action == "open_url" || action == "open_app") {
            if (action == "open_url" || target.find("http://") == 0 || target.find("https://") == 0) {
                openOnDesktop(target);
                return "{\"success\":true,\"opened_url\":\"" + jsonEscape(target) + "\"}";
            }
            std::string lower = target;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("chrome") != std::string::npos || lower.find("browser") != std::string::npos) {
                spawnDetachedProcess({"google-chrome"});
            } else if (lower.find("files") != std::string::npos || lower.find("nautilus") != std::string::npos) {
                openOnDesktop(getHomeDir() + "/Documents");
            } else if (lower.find("notes") != std::string::npos) {
                openOnDesktop(getNotesDir());
            } else {
                spawnDetachedProcess({target});
            }
            return "{\"success\":true,\"launched\":\"" + jsonEscape(target) + "\"}";
        } else if (action == "copy_clipboard") {
            copyToClipboard(target);
            return "{\"success\":true,\"copied_bytes\":" + std::to_string(target.size()) + "}";
        } else if (action == "run_command") {
            std::string out = runCommandCapture(target + " 2>&1", 8192);
            return "{\"success\":true,\"output\":\"" + jsonEscape(out) + "\"}";
        }
        return "{\"error\":\"Unknown computer_control_action: " + jsonEscape(action) + "\"}";
    }

    std::string webSearchAndFetch(const std::string &queryOrUrl) {
        if (queryOrUrl.find("http://") == 0 || queryOrUrl.find("https://") == 0) {
            // SSRF guard: refuse loopback, link-local and private-network targets
            std::string lower = queryOrUrl;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            auto hostStart = lower.find("://") + 3;
            auto hostEnd = lower.find('/', hostStart);
            std::string host = lower.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
            auto atPos = host.rfind('@'); // strip userinfo
            if (atPos != std::string::npos) host = host.substr(atPos + 1);
            auto colonPos = host.rfind(':');
            if (colonPos != std::string::npos && host.find(':') == colonPos) host = host.substr(0, colonPos);
            static const char *blocked[] = {
                "localhost", "127.", "0.0.0.0", "::1", "10.", "192.168.", "169.254.",
                "172.16.", "172.17.", "172.18.", "172.19.", "172.20.", "172.21.", "172.22.",
                "172.23.", "172.24.", "172.25.", "172.26.", "172.27.", "172.28.", "172.29.",
                "172.30.", "172.31.", "metadata.google", "100.100."
            };
            for (const char *b : blocked) {
                if (host == b || host.rfind(b, 0) == 0 || host.find(b) == (host.size() > strlen(b) ? host.size() - strlen(b) : 0)) {
                    return "{\"error\":\"url_blocked\",\"message\":\"Refusing to fetch loopback/private/link-local addresses.\"}";
                }
            }
            std::string cmd = "curl -sL -m 8 " + shellSingleQuote(queryOrUrl) +
                              " | sed -e 's/<script[^>]*>.*<\\/script>//gI' -e 's/<style[^>]*>.*<\\/style>//gI' -e 's/<[^>]*>/ /g' | tr -s ' \\n\\t' ' ' | head -c 4000";
            std::string text = runCommandCapture(cmd, 4096);
            return "{\"url\":\"" + jsonEscape(queryOrUrl) + "\",\"content_excerpt\":\"" + jsonEscape(text) + "\"}";
        }
        std::string ddgUrl = "https://html.duckduckgo.com/html/?q=" + urlEncode(queryOrUrl);
        // grep -P lookbehind with variable-length pattern is rejected by PCRE; use sed instead
        std::string cmd = "curl -sL -m 8 -A 'Mozilla/5.0' " + shellSingleQuote(ddgUrl) +
                          " | sed -n 's/.*class=\"result__snippet\"[^>]*>//p' | sed 's/<[^>]*>//g' | head -n 8";
        std::string snippets = runCommandCapture(cmd, 4096);
        if (snippets.empty()) {
            std::string fallbackCmd = "curl -sL -m 8 -A 'Mozilla/5.0' " + shellSingleQuote(ddgUrl) +
                                      " | sed -e 's/<[^>]*>/ /g' | tr -s ' ' | head -c 2500";
            snippets = runCommandCapture(fallbackCmd, 3000);
        }
        return "{\"query\":\"" + jsonEscape(queryOrUrl) + "\",\"results\":\"" + jsonEscape(snippets) + "\"}";
    }

    std::string scheduleTask(const std::string &title, const std::string &taskType,
                             const std::string &targetTimeStr, const std::string &details) {
        time_t targetEpoch = 0;
        int secondsUntil = 0;
        std::string formattedTarget;
        if (!parseTargetTime(targetTimeStr, 0, targetEpoch, secondsUntil, formattedTarget)) {
            return std::string("{\"error\":\"unparseable_time\",\"message\":\"Could not understand the time '\") +
                   jsonEscape(targetTimeStr) + "'. Give a clear time (e.g. '2026-10-15 09:00', 'tomorrow 8 AM'). No task was scheduled.\"}";
        }
        if (secondsUntil < 1) secondsUntil = 1;

        std::string id = "task-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(rand() % 1000);
        std::string nowStr = currentReadableTimestamp();

        const char *sql =
            "INSERT INTO scheduled_tasks (id, title, task_type, target_time, target_epoch, details, status, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, 'scheduled', ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, taskType.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, formattedTarget.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(targetEpoch));
            sqlite3_bind_text(stmt, 6, details.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Also add to GNOME Calendar and arm background execution thread
        appendIcsEvent(id, title, formattedTarget, taskType, details);
        sendDesktopNotification("🗓️ Task Scheduled: " + title, "Will execute at " + formattedTarget);

        emitRichCard({
            "alarm",
            "🗓️ Scheduled Task: " + title,
            taskType,
            "Target: " + formattedTarget + " (in " + std::to_string(secondsUntil) + "s)",
            details.empty() ? "Task armed in background scheduler." : details,
            "",
            ""
        });

        std::thread([this, id, title, taskType, formattedTarget, details, secondsUntil]() {
            int waitSec = std::min(secondsUntil, 86400);
            std::this_thread::sleep_for(std::chrono::seconds(waitSec));
            {
                std::lock_guard<std::recursive_mutex> lk(dbMutex_);
                if (db_) {
                    std::string upd = "UPDATE scheduled_tasks SET status='executed' WHERE id='" + jsonEscape(id) + "';";
                    sqlite3_exec(db_, upd.c_str(), nullptr, nullptr, nullptr);
                }
            }
            sendDesktopNotification("🚀 Executing Scheduled Task: " + title, details.empty() ? formattedTarget : details, true);
            LinuxAudioEngine::instance().playAlarmChime();

            if (taskType == "linkedin_post") {
                std::string url = "https://www.linkedin.com/feed/?shareActive=true&text=" + urlEncode(details);
                openOnDesktop(url);
            } else if (taskType == "screencast") {
                std::lock_guard<std::recursive_mutex> lk(dbMutex_);
                recordScreencast(title, 5, "");
            }
        }).detach();

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"task_id\":\"" << jsonEscape(id) << "\","
            << "\"title\":\"" << jsonEscape(title) << "\","
            << "\"task_type\":\"" << jsonEscape(taskType) << "\","
            << "\"scheduled_for\":\"" << jsonEscape(formattedTarget) << "\","
            << "\"seconds_until_execution\":" << secondsUntil << ","
            << "\"status\":\"scheduled\""
            << "}";
        return out.str();
    }

    std::string saveMemory(const std::string &key, const std::string &value, const std::string &category) {
        std::string nowStr = currentReadableTimestamp();
        std::string cat = category.empty() ? "preference" : category;
        const char *sql =
            "INSERT INTO memories (key, value, category, updated_at) VALUES (?, ?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value, category=excluded.category, updated_at=excluded.updated_at;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, cat.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Export all memories to ~/Documents/AmuseNotes/Amuse_Active_Memory.md
        std::ostringstream md;
        sqlite3_stmt *qstmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT key, value, category, updated_at FROM memories ORDER BY id ASC;", -1, &qstmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(qstmt) == SQLITE_ROW) {
                std::string k = reinterpret_cast<const char *>(sqlite3_column_text(qstmt, 0)) ?: "";
                std::string v = reinterpret_cast<const char *>(sqlite3_column_text(qstmt, 1)) ?: "";
                std::string c = reinterpret_cast<const char *>(sqlite3_column_text(qstmt, 2)) ?: "";
                md << "- **[" << c << "] " << k << "**: " << v << "\n";
            }
            sqlite3_finalize(qstmt);
        }
        std::string memPath = writeNoteMarkdownFile("Amuse_Active_Memory", nowStr, md.str(), true);

        emitRichCard({
            "note",
            "🧠 Memory Saved: " + key,
            cat,
            memPath,
            value,
            memPath,
            "Open Memory File"
        });

        return "{\"success\":true,\"key\":\"" + jsonEscape(key) + "\",\"value\":\"" + jsonEscape(value) +
               "\",\"category\":\"" + jsonEscape(cat) + "\",\"memory_file\":\"" + jsonEscape(memPath) + "\"}";
    }

    std::string recallMemories(const std::string &query) {
        std::string sql = "SELECT key, value, category, updated_at FROM memories ";
        if (!query.empty()) {
            sql += "WHERE key LIKE ? COLLATE NOCASE OR value LIKE ? COLLATE NOCASE OR category LIKE ? COLLATE NOCASE ";
        }
        sql += "ORDER BY id DESC LIMIT 30;";
        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"memories\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (!query.empty()) {
                std::string q = "%" + query + "%";
                sqlite3_bind_text(stmt, 1, q.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, q.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, q.c_str(), -1, SQLITE_TRANSIENT);
            }
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                out << "{"
                    << "\"key\":\"" << jsonEscape(col(0)) << "\","
                    << "\"value\":\"" << jsonEscape(col(1)) << "\","
                    << "\"category\":\"" << jsonEscape(col(2)) << "\","
                    << "\"updated_at\":\"" << jsonEscape(col(3)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string manageActionPlan(const std::string &action, const std::string &goal,
                                 const std::string &steps, const std::string &status) {
        std::string nowStr = currentReadableTimestamp();
        if (action == "create" || action == "update") {
            std::string id = "plan-" + sanitizeFilename(goal);
            std::string st = status.empty() ? "in_progress" : status;
            const char *sql =
                "INSERT INTO action_plans (id, goal, steps, status, updated_at) VALUES (?, ?, ?, ?, ?) "
                "ON CONFLICT(id) DO UPDATE SET steps=excluded.steps, status=excluded.status, updated_at=excluded.updated_at;";
            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, goal.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, steps.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, st.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, nowStr.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            std::string planPath = writeNoteMarkdownFile("Action_Plan_" + goal, nowStr, steps, true);
            emitRichCard({
                "note",
                "📋 Action Plan: " + goal,
                st,
                planPath,
                steps,
                planPath,
                "Open Plan Markdown"
            });
            return "{\"success\":true,\"plan_id\":\"" + jsonEscape(id) + "\",\"goal\":\"" + jsonEscape(goal) +
                   "\",\"status\":\"" + jsonEscape(st) + "\",\"file_path\":\"" + jsonEscape(planPath) + "\"}";
        }

        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"action_plans\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, "SELECT id, goal, steps, status, updated_at FROM action_plans ORDER BY updated_at DESC LIMIT 15;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) out << ",";
                first = false;
                auto col = [&](int idx) -> std::string {
                    const char *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
                    return v ? v : "";
                };
                out << "{"
                    << "\"id\":\"" << jsonEscape(col(0)) << "\","
                    << "\"goal\":\"" << jsonEscape(col(1)) << "\","
                    << "\"steps\":\"" << jsonEscape(col(2)) << "\","
                    << "\"status\":\"" << jsonEscape(col(3)) << "\","
                    << "\"updated_at\":\"" << jsonEscape(col(4)) << "\""
                    << "}";
            }
            sqlite3_finalize(stmt);
        }
        out << "]}";
        return out.str();
    }

    std::string buildAndRunCustomTool(const std::string &toolName, const std::string &language,
                                      const std::string &code, const std::string &description) {
        std::string toolsDir = getHomeDir() + "/.amuse/custom_tools";
        mkdirRecursive(toolsDir);
        bool isPy = (language != "bash" && language != "sh");
        std::string ext = isPy ? ".py" : ".sh";
        std::string safeName = sanitizeFilename(toolName);
        std::string scriptPath = toolsDir + "/" + safeName + ext;

        std::ofstream ofs(scriptPath, std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << code << "\n";
            ofs.close();
            chmod(scriptPath.c_str(), 0755);
        }

        std::string nowStr = currentReadableTimestamp();
        const char *sql =
            "INSERT INTO custom_tools (name, language, script_path, description, created_at) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(name) DO UPDATE SET script_path=excluded.script_path, description=excluded.description, created_at=excluded.created_at;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, safeName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, isPy ? "python" : "bash", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, scriptPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, description.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, nowStr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // 30s hard timeout so a hanging script can't freeze every other tool
        std::string output = runProcessWithIO({isPy ? "/usr/bin/python3" : "/bin/bash", scriptPath}, "", 8192, 30);

        emitRichCard({
            "connector",
            "🛠️ Custom Tool Built & Executed: " + safeName,
            isPy ? "Python" : "Bash",
            scriptPath,
            output.empty() ? "(Executed with no stdout)" : output,
            scriptPath,
            "Open Script"
        });

        return "{\"success\":true,\"tool_name\":\"" + jsonEscape(safeName) + "\",\"script_path\":\"" +
               jsonEscape(scriptPath) + "\",\"output\":\"" + jsonEscape(output) + "\"}";
    }

    std::string mediaAndSystemControl(const std::string &action) {
        std::string out;
        if (action == "volume_up") {
            out = runCommandCapture("pactl set-sink-volume @DEFAULT_SINK@ +10% 2>&1", 512);
        } else if (action == "volume_down") {
            out = runCommandCapture("pactl set-sink-volume @DEFAULT_SINK@ -10% 2>&1", 512);
        } else if (action == "mute_toggle") {
            out = runCommandCapture("pactl set-sink-mute @DEFAULT_SINK@ toggle 2>&1", 512);
        } else if (action == "media_play_pause") {
            out = runCommandCapture("dbus-send --print-reply --dest=org.mpris.MediaPlayer2.spotify /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.PlayPause 2>&1", 512);
        } else if (action == "media_next") {
            out = runCommandCapture("dbus-send --print-reply --dest=org.mpris.MediaPlayer2.spotify /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.Next 2>&1", 512);
        } else if (action == "media_prev") {
            out = runCommandCapture("dbus-send --print-reply --dest=org.mpris.MediaPlayer2.spotify /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.Previous 2>&1", 512);
        } else if (action == "open_spotify") {
            openOnDesktop("https://open.spotify.com");
            out = "Opened Spotify";
        } else {
            out = runCommandCapture("xprop -root _NET_ACTIVE_WINDOW 2>/dev/null | awk '{print $NF}' | xargs -I{} xprop -id {} WM_NAME WM_CLASS 2>/dev/null", 1024);
        }
        return "{\"success\":true,\"action\":\"" + jsonEscape(action) + "\",\"result\":\"" + jsonEscape(out) + "\"}";
    }

    std::string browserAgentAction(const std::string &argsJson) {
        JsonHelper args(argsJson);
        std::string action = args.getString("action", "navigate_and_extract");
        std::string url = args.getString("url", "");
        std::string scriptPath = getRepoRootDir() + "/scripts/browser_agent.py";
        std::string pyBin = getHomeDir() + "/.pyenv/shims/python3";
        struct stat st{};
        if (stat(pyBin.c_str(), &st) != 0) pyBin = "/usr/bin/python3";

        std::string cmd = shellSingleQuote(pyBin) + " " + shellSingleQuote(scriptPath) + " " +
                          shellSingleQuote(argsJson.empty() ? "{\"action\":\"navigate_and_extract\"}" : argsJson) + " 2>/dev/null";
        std::string outJson = runCommandCapture(cmd, 32768);
        while (!outJson.empty() && (outJson.back() == '\n' || outJson.back() == '\r')) outJson.pop_back();
        if (outJson.empty() || outJson[0] != '{') {
            outJson = "{\"success\":false,\"error\":\"Browser agent failed\"}";
        }

        JsonHelper parsed(outJson);
        std::string shotPath = parsed.getString("screenshot_path", "");
        std::string title = parsed.getString("title", url);
        if (!shotPath.empty()) {
            emitRichCard({
                "media",
                "🌐 Browser Agent: " + title,
                action,
                shotPath,
                "Automated browser action (" + action + ") on " + url,
                shotPath,
                "View Page Capture"
            });
        } else {
            emitRichCard({
                "connector",
                "🌐 Browser Agent: " + title,
                action,
                url,
                "Executed browser action (" + action + ") on " + url,
                url,
                "Open URL"
            });
        }
        return outJson;
    }

    std::string guiDesktopAction(const std::string &argsJson) {
        JsonHelper args(argsJson);
        std::string action = args.getString("action", "hotkey");
        int x = args.getInt("x", 640);
        int y = args.getInt("y", 400);
        std::string textOrKey = args.getString("text_or_key", "");
        bool ok = false;
        std::string detail;

        if (action == "click") {
            ok = x11MoveAndClick(x, y, 1, true);
            detail = "Clicked left mouse button at (" + std::to_string(x) + ", " + std::to_string(y) + ")";
        } else if (action == "move_mouse") {
            ok = x11MoveAndClick(x, y, 1, false);
            detail = "Moved mouse cursor to (" + std::to_string(x) + ", " + std::to_string(y) + ")";
        } else if (action == "hotkey") {
            ok = x11SendKeyCombo(textOrKey.empty() ? "Return" : textOrKey);
            detail = "Sent X11 key combo: " + textOrKey;
        } else if (action == "type_text") {
            copyToClipboard(textOrKey);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            ok = x11SendKeyCombo("Ctrl+v");
            detail = "Copied & pasted text into active window (" + std::to_string(textOrKey.size()) + " chars)";
        } else if (action == "paste_image") {
            std::string imgPath = textOrKey.empty() ? lastScreenshotPath_ : expandTilde(textOrKey);
            copyImageToClipboard(imgPath);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ok = x11SendKeyCombo("Ctrl+v");
            detail = "Copied PNG image to X11 clipboard and pasted via Ctrl+V: " + imgPath;
        }

        emitRichCard({
            "connector",
            "🖱️ GUI Desktop Action: " + action,
            ok ? "EXECUTED" : "ERROR",
            "X11 libXtst Desktop Automation",
            detail,
            "",
            ""
        });

        std::ostringstream out;
        out << "{"
            << "\"success\":" << (ok ? "true" : "false") << ","
            << "\"action\":\"" << jsonEscape(action) << "\","
            << "\"detail\":\"" << jsonEscape(detail) << "\""
            << "}";
        return out.str();
    }
};

// ============================================================================
// Global Double-Tap Option/Alt Hotkey Monitor (X11 XInput2)
// ============================================================================

class LinuxHotkeyMonitor {
public:
    static LinuxHotkeyMonitor &instance() {
        static LinuxHotkeyMonitor inst;
        return inst;
    }

    void start(std::function<void()> onTrigger) {
        if (running_) return;
        onTrigger_ = onTrigger;
        running_ = true;
        thread_ = std::thread([this]() { runLoop(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::function<void()> onTrigger_;

    void runLoop() {
        Display *dpy = XOpenDisplay(nullptr);
        if (!dpy) {
            std::cerr << "[AmuseHotkey] Could not open X11 display for global hotkey monitor." << std::endl;
            return;
        }

        int xiOpcode = 0, event = 0, error = 0;
        if (!XQueryExtension(dpy, "XInputExtension", &xiOpcode, &event, &error)) {
            std::cerr << "[AmuseHotkey] XInput2 extension not available." << std::endl;
            XCloseDisplay(dpy);
            return;
        }

        Window root = DefaultRootWindow(dpy);
        XIEventMask evmask;
        unsigned char mask[(XI_LASTEVENT + 7) / 8] = {0};
        evmask.deviceid = XIAllMasterDevices;
        evmask.mask_len = sizeof(mask);
        evmask.mask = mask;
        XISetMask(mask, XI_RawKeyPress);
        XISetMask(mask, XI_RawKeyRelease);
        XISelectEvents(dpy, root, &evmask, 1);
        XFlush(dpy);

        KeyCode altL = XKeysymToKeycode(dpy, XK_Alt_L);
        KeyCode altR = XKeysymToKeycode(dpy, XK_Alt_R);
        KeyCode metaL = XKeysymToKeycode(dpy, XK_Meta_L);
        KeyCode metaR = XKeysymToKeycode(dpy, XK_Meta_R);

        bool wasAltDown = false;
        double lastAltPressTime = 0.0;

        int x11Fd = ConnectionNumber(dpy);
        while (running_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(x11Fd, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms poll timeout

            if (XPending(dpy) > 0 || select(x11Fd + 1, &fds, nullptr, nullptr, &tv) > 0) {
                while (XPending(dpy) > 0) {
                    XEvent ev;
                    XNextEvent(dpy, &ev);
                    if (ev.xcookie.type == GenericEvent && ev.xcookie.extension == xiOpcode &&
                        XGetEventData(dpy, &ev.xcookie)) {
                        int evtype = ev.xcookie.evtype;
                        if (evtype == XI_RawKeyPress || evtype == XI_RawKeyRelease) {
                            XIRawEvent *raw = static_cast<XIRawEvent *>(ev.xcookie.data);
                            KeyCode kc = static_cast<KeyCode>(raw->detail);
                            bool isAlt = (kc == altL || kc == altR || kc == metaL || kc == metaR);

                            auto now = std::chrono::steady_clock::now();
                            double nowSec = std::chrono::duration<double>(now.time_since_epoch()).count();

                            if (!isAlt && evtype == XI_RawKeyPress) {
                                wasAltDown = false;
                                lastAltPressTime = 0.0;
                            } else if (isAlt) {
                                if (evtype == XI_RawKeyPress && !wasAltDown) {
                                    wasAltDown = true;
                                    if (nowSec - lastAltPressTime < 0.35) {
                                        lastAltPressTime = 0.0;
                                        if (onTrigger_) onTrigger_();
                                    } else {
                                        lastAltPressTime = nowSec;
                                    }
                                } else if (evtype == XI_RawKeyRelease) {
                                    wasAltDown = false;
                                }
                            }
                        }
                        XFreeEventData(dpy, &ev.xcookie);
                    }
                }
            }
        }

        XCloseDisplay(dpy);
    }
};

// ============================================================================
// Amuse GTK3 + WebKit2GTK Floating HUD & Node Orchestrator Controller
// ============================================================================

// Find a usable node binary: AMUSE_NODE env → nvm → common paths → PATH.
static std::string findNodeBinary() {
    const char *envNode = getenv("AMUSE_NODE");
    if (envNode && *envNode && access(envNode, X_OK) == 0) return envNode;

    // nvm: ~/.nvm/versions/node/v*/bin/node (lexically last = newest version)
    std::string nvmRoot = getHomeDir() + "/.nvm/versions/node";
    DIR *d = opendir(nvmRoot.c_str());
    if (d) {
        std::string latest;
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == 'v') {
                std::string p = nvmRoot + "/" + ent->d_name + "/bin/node";
                if (access(p.c_str(), X_OK) == 0) latest = p;
            }
        }
        closedir(d);
        if (!latest.empty()) return latest;
    }

    if (access("/usr/local/bin/node", X_OK) == 0) return "/usr/local/bin/node";
    if (access("/usr/bin/node", X_OK) == 0) return "/usr/bin/node";
    if (access("/snap/bin/node", X_OK) == 0) return "/snap/bin/node";

    // Volta: ~/.volta/tools/image/node/<version>/bin/node
    std::string voltaRoot = getHomeDir() + "/.volta/tools/image/node";
    DIR *vd = opendir(voltaRoot.c_str());
    if (vd) {
        std::string latest;
        struct dirent *ent;
        while ((ent = readdir(vd)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string p = voltaRoot + "/" + ent->d_name + "/bin/node";
            if (access(p.c_str(), X_OK) == 0) latest = p;
        }
        closedir(vd);
        if (!latest.empty()) return latest;
    }

    // Last resort: resolve via login shell PATH
    std::string which = runCommandCapture("bash -lc 'command -v node' 2>/dev/null", 256);
    while (!which.empty() && (which.back() == '\n' || which.back() == '\r')) which.pop_back();
    if (!which.empty() && access(which.c_str(), X_OK) == 0) return which;
    return "";
}

// Read provider from ~/.amuse/config.json ("gemini" | "openai" | "").
static std::string readConfigProvider() {
    std::string cfgPath = getHomeDir() + "/.amuse/config.json";
    std::ifstream ifs(cfgPath);
    if (!ifs.is_open()) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    JsonHelper cfg(ss.str());
    return cfg.getString("provider", "");
}

class AmuseAppController {
public:
    static AmuseAppController &instance() {
        static AmuseAppController inst;
        return inst;
    }

    void run(int argc, char **argv) {
        std::cerr << "[Amuse] Native controller launching..." << std::endl;
        gtk_init(&argc, &argv);

        // Ensure local DB and ~/Documents/AmuseNotes are initialized & synced on startup
        LocalConnectorsManager::instance().setRichCardCallback([this](const RichCardEvent &card) {
            appendRichCard(card);
        });
        LocalConnectorsManager::instance().setImageFrameCallback([this](const std::string &b64, const std::string &mime) {
            sendToNode("{\"type\":\"image_input_frame\",\"mimeType\":\"" + jsonEscape(mime) + "\",\"data\":\"" + b64 + "\"}");
        });
        LocalConnectorsManager::instance().executeTool("get_current_time_and_environment", "{}");

        setupHUDWindow();
        launchNodeOrchestrator();

        LinuxHotkeyMonitor::instance().start([this]() {
            std::cerr << "[Amuse] Double-tap Option/Alt detected! Toggling HUD." << std::endl;
            g_idle_add(+[](gpointer data) -> gboolean {
                static_cast<AmuseAppController *>(data)->toggleHUD();
                return G_SOURCE_REMOVE;
            }, this);
        });

        showHUD();
        setStatus("idle", "Press \u2325\u2325 (Double-tap Alt/Option) anytime to toggle.");

        if (const char *startupPrompt = getenv("AMUSE_STARTUP_PROMPT")) {
            std::string promptStr(startupPrompt);
            if (!promptStr.empty()) {
                appendTranscript("user", promptStr);
                sendToNode("{\"type\":\"user_text_input\",\"text\":\"" + jsonEscape(promptStr) + "\"}");
            }
        }

        if (const char *autoExitMs = getenv("AMUSE_AUTO_EXIT_MS")) {
            int ms = std::atoi(autoExitMs);
            if (ms > 0) {
                g_timeout_add(ms, +[](gpointer) -> gboolean {
                    gtk_main_quit();
                    return G_SOURCE_REMOVE;
                }, nullptr);
            }
        }

        gtk_main();

        LinuxHotkeyMonitor::instance().stop();
        LinuxAudioEngine::instance().stopRecording();
        if (nodePid_ > 0) {
            kill(nodePid_, SIGTERM);
        }
    }

    void showHUD() {
        if (!window_) return;
        gtk_widget_show_all(window_);
        gtk_window_present(GTK_WINDOW(window_));
        isVisible_ = true;
        evalJS("window.onHUDVisibilityChanged && window.onHUDVisibilityChanged(true);");
    }

    void hideHUD() {
        if (!window_) return;
        gtk_widget_hide(window_);
        isVisible_ = false;
        evalJS("window.onHUDVisibilityChanged && window.onHUDVisibilityChanged(false);");
    }

    void toggleHUD() {
        if (isVisible_) {
            hideHUD();
        } else {
            showHUD();
        }
    }

    void setStatus(const std::string &status, const std::string &details) {
        std::string js = "window.setAgentStatus && window.setAgentStatus(\"" +
                         jsonEscape(status) + "\", \"" + jsonEscape(details) + "\");";
        evalJS(js);
    }

    void appendTranscript(const std::string &role, const std::string &text, const std::string &toolName = "") {
        std::string obj = "{\"role\":\"" + jsonEscape(role) + "\",\"text\":\"" + jsonEscape(text) + "\"";
        if (!toolName.empty()) {
            obj += ",\"toolName\":\"" + jsonEscape(toolName) + "\"";
        }
        obj += "}";
        evalJS("window.appendTranscriptMessage && window.appendTranscriptMessage(" + obj + ");");
    }

    void updateTranscriptDelta(const std::string &role, const std::string &text) {
        std::string obj = "{\"role\":\"" + jsonEscape(role) + "\",\"text\":\"" + jsonEscape(text) + "\"}";
        evalJS("window.updateTranscriptDelta && window.updateTranscriptDelta(" + obj + ");");
    }

    void appendRichCard(const RichCardEvent &card) {
        std::string obj = "{"
            "\"cardType\":\"" + jsonEscape(card.cardType) + "\","
            "\"title\":\"" + jsonEscape(card.title) + "\","
            "\"badge\":\"" + jsonEscape(card.badge) + "\","
            "\"subtitle\":\"" + jsonEscape(card.subtitle) + "\","
            "\"body\":\"" + jsonEscape(card.body) + "\","
            "\"filePath\":\"" + jsonEscape(card.filePath) + "\","
            "\"actionLabel\":\"" + jsonEscape(card.actionLabel) + "\""
            "}";
        evalJS("window.appendRichCard && window.appendRichCard(" + obj + ");");
    }

    void updateAudioLevel(float level) {
        char buf[128];
        snprintf(buf, sizeof(buf), "window.updateAudioLevel && window.updateAudioLevel(%f);", level);
        evalJS(buf);
    }

    void onPageLoaded() {
        std::lock_guard<std::mutex> lock(jsMutex_);
        pageLoaded_ = true;
        for (const auto &js : pendingJs_) {
            webkit_web_view_evaluate_javascript(webView_, js.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        pendingJs_.clear();
    }

    void handleWebMessage(const std::string &jsonStr) {
        JsonHelper msg(jsonStr);
        std::string type = msg.getString("type", "");
        std::string payloadJson = msg.getObjectJson("payload", "{}");
        JsonHelper payload(payloadJson);

        if (type == "hideHUD") {
            hideHUD();
        } else if (type == "startDrag") {
            if (window_) {
                int button = payload.getInt("button", 1);
                int screenX = payload.getInt("screenX", 0);
                int screenY = payload.getInt("screenY", 0);
                gtk_window_begin_move_drag(GTK_WINDOW(window_), button, screenX, screenY, GDK_CURRENT_TIME);
            }
        } else if (type == "toggleVoice") {
            bool active = payload.getBool("active", false);
            if (active) {
                startAudioCapture();
            } else {
                stopAudioCapture();
            }
        } else if (type == "sendTextMessage") {
            std::string text = payload.getString("text", "");
            if (!text.empty()) {
                std::cerr << "[Amuse] User text input from HUD (" << text.size() << " chars)" << std::endl;
                sendToNode("{\"type\":\"user_text_input\",\"text\":\"" + jsonEscape(text) + "\"}");
            }
        } else if (type == "openFile") {
            std::string path = payload.getString("path", "");
            if (!path.empty()) {
                openOnDesktop(path);
            }
        } else if (type == "showNotesViewer") {
            LocalConnectorsManager::instance().emitAllNotesToHUD();
        } else if (type == "showConnectorsStatus") {
            LocalConnectorsManager::instance().emitConnectorsSummaryToHUD();
        } else if (type == "showMemoryAndPlans") {
            LocalConnectorsManager::instance().emitMemoryAndPlansToHUD();
        } else if (type == "shareScreenSnapshot") {
            std::thread([this]() {
                LocalConnectorsManager::instance().executeTool("take_screenshot", "{\"title\":\"Live_Screen_Vision\"}");
                sendToNode("{\"type\":\"user_text_input\",\"text\":\"I just shared a live screenshot of my screen with you. Tell me what you see on my screen right now.\"}");
            }).detach();
        } else if (type == "runDirectTool") {
            std::string toolName = payload.getString("toolName", "");
            std::string argsStr = payload.getObjectJson("args", "{}");
            if (!toolName.empty()) {
                std::thread([this, toolName, argsStr]() {
                    setStatus("thinking", "Executing " + toolName + "...");
                    LocalConnectorsManager::instance().executeTool(toolName, argsStr);
                    appendTranscript("tool", "Connected & executed " + toolName, toolName);
                    setStatus("idle", "Ready");
                }).detach();
            }
        }
    }

private:
    GtkWidget *window_ = nullptr;
    WebKitWebView *webView_ = nullptr;
    bool isVisible_ = false;

    std::mutex jsMutex_;
    bool pageLoaded_ = false;
    std::vector<std::string> pendingJs_;

    pid_t nodePid_ = -1;
    int nodeStdinFd_ = -1;
    std::mutex nodeWriteMutex_;
    std::thread nodeReadThread_;

    void setupHUDWindow() {
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), "Amuse HUD");
        gtk_window_set_default_size(GTK_WINDOW(window_), 520, 700);
        gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(window_), TRUE);
        gtk_window_stick(GTK_WINDOW(window_));
        gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
        gtk_widget_set_app_paintable(window_, TRUE);

        GdkScreen *screen = gtk_widget_get_screen(window_);
        GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
        if (visual && gdk_screen_is_composited(screen)) {
            gtk_widget_set_visual(window_, visual);
        }

        g_signal_connect(window_, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

        WebKitUserContentManager *ucm = webkit_user_content_manager_new();
        g_signal_connect(ucm, "script-message-received::amuseBridge",
                         G_CALLBACK(+[](WebKitUserContentManager *, WebKitJavascriptResult *res, gpointer data) {
                             JSCValue *val = webkit_javascript_result_get_js_value(res);
                             if (val) {
                                 char *json = jsc_value_to_json(val, 0);
                                 if (json) {
                                     static_cast<AmuseAppController *>(data)->handleWebMessage(json);
                                     g_free(json);
                                 }
                             }
                         }),
                         this);
        webkit_user_content_manager_register_script_message_handler(ucm, "amuseBridge");

        WebKitSettings *settings = webkit_settings_new();
        webkit_settings_set_enable_developer_extras(settings, TRUE);
        webkit_settings_set_enable_webaudio(settings, TRUE);
        webkit_settings_set_enable_media_stream(settings, TRUE);

        webView_ = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
        webkit_web_view_set_settings(webView_, settings);

        GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
        webkit_web_view_set_background_color(webView_, &transparent);

        g_signal_connect(webView_, "load-changed",
                         G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent loadEvent, gpointer data) {
                             if (loadEvent == WEBKIT_LOAD_FINISHED) {
                                 static_cast<AmuseAppController *>(data)->onPageLoaded();
                             }
                         }),
                         this);

        gtk_container_add(GTK_CONTAINER(window_), GTK_WIDGET(webView_));

        char cwdBuf[PATH_MAX];
        std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
        std::string htmlUri = "file://" + cwd + "/src/web/index.html";
        webkit_web_view_load_uri(webView_, htmlUri.c_str());
    }

    void evalJS(const std::string &js) {
        std::string *heapJs = new std::string(js);
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            +[](gpointer data) -> gboolean {
                std::string *str = static_cast<std::string *>(data);
                AmuseAppController &ctrl = AmuseAppController::instance();
                std::lock_guard<std::mutex> lock(ctrl.jsMutex_);
                if (ctrl.pageLoaded_ && ctrl.webView_) {
                    webkit_web_view_evaluate_javascript(ctrl.webView_, str->c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
                } else {
                    ctrl.pendingJs_.push_back(*str);
                }
                delete str;
                return G_SOURCE_REMOVE;
            },
            heapJs,
            nullptr
        );
    }

    void startAudioCapture() {
        std::string err;
        bool ok = LinuxAudioEngine::instance().startRecording(
            [this](const uint8_t *pcm, size_t len, float level) {
                std::string b64 = base64Encode(pcm, len);
                sendToNode("{\"type\":\"audio_input_chunk\",\"data\":\"" + b64 + "\"}");
                updateAudioLevel(level);
            },
            err
        );
        if (ok) {
            setStatus("listening", "Microphone active. Streaming to Gemini...");
        } else {
            setStatus("idle", "Microphone error: " + err);
        }
    }

    void stopAudioCapture() {
        LinuxAudioEngine::instance().stopRecording();
        setStatus("idle", "Voice muted. Click mic or type below.");
        sendToNode("{\"type\":\"voice_stopped\"}");
    }

    void launchNodeOrchestrator() {
        int stdinPipe[2];
        int stdoutPipe[2];
        if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0) {
            std::cerr << "[Amuse] Failed to create pipes for Node orchestrator" << std::endl;
            return;
        }

        char cwdBuf[PATH_MAX];
        std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";

        // Choose orchestrator: AMUSE_ORCHESTRATOR env override, else config provider
        // (provider=openai → hatch any-backend loop; default → legacy Gemini Live client)
        const char *orchEnv = getenv("AMUSE_ORCHESTRATOR");
        std::string scriptName = "index.mjs";
        bool useHatch = (orchEnv && std::string(orchEnv) == "hatch");
        if (!orchEnv || !*orchEnv) {
            if (readConfigProvider() == "openai") useHatch = true;
        }
        if (useHatch) scriptName = "hatchOrchestrator.mjs";

        std::string scriptPath = cwd + "/src/js/" + scriptName;

        std::string nodeBin = findNodeBinary();
        if (nodeBin.empty()) {
            std::cerr << "[Amuse] ERROR: node binary not found. Set AMUSE_NODE=/path/to/node" << std::endl;
            setStatus("error", "node not found — set AMUSE_NODE env var");
            return;
        }

        pid_t pid = fork();
        if (pid == 0) {
            dup2(stdinPipe[0], STDIN_FILENO);
            dup2(stdoutPipe[1], STDOUT_FILENO);
            close(stdinPipe[0]);
            close(stdinPipe[1]);
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);

            execl(nodeBin.c_str(), "node", scriptPath.c_str(), nullptr);
            _exit(127);
        } else if (pid > 0) {
            nodePid_ = pid;
            std::cerr << "[Amuse] Node orchestrator launched (PID: " << pid << ")" << std::endl;
            close(stdinPipe[0]);
            close(stdoutPipe[1]);
            nodeStdinFd_ = stdinPipe[1];
            int readFd = stdoutPipe[0];

            nodeReadThread_ = std::thread([this, readFd]() {
                std::string buffer;
                char chunk[4096];
                ssize_t n;
                while ((n = read(readFd, chunk, sizeof(chunk))) > 0) {
                    buffer.append(chunk, static_cast<size_t>(n));
                    size_t pos;
                    while ((pos = buffer.find('\n')) != std::string::npos) {
                        std::string line = buffer.substr(0, pos);
                        buffer.erase(0, pos + 1);
                        if (!line.empty()) {
                            handleNodeMessageLine(line);
                        }
                    }
                }
                close(readFd);
            });
            nodeReadThread_.detach();
        }
    }

    void sendToNode(const std::string &jsonLine) {
        std::lock_guard<std::mutex> lock(nodeWriteMutex_);
        if (nodeStdinFd_ < 0) return;
        std::string data = jsonLine + "\n";
        ssize_t written = write(nodeStdinFd_, data.c_str(), data.size());
        (void)written;
    }

    void handleNodeMessageLine(const std::string &line) {
        JsonHelper msg(line);
        if (!msg.isValidObject()) return;

        std::string type = msg.getString("type", "");
        if (type == "audio_output_chunk") {
            std::string b64 = msg.getString("data", "");
            if (!b64.empty()) {
                std::vector<uint8_t> pcm = base64Decode(b64);
                LinuxAudioEngine::instance().enqueuePlayback(pcm, [this](bool playing) {
                    if (playing) {
                        std::cerr << "[Amuse] Playing 24kHz PCM audio output via PulseAudio/PipeWire..." << std::endl;
                        setStatus("speaking", "Amuse is speaking...");
                    } else {
                        setStatus("idle", "Ready");
                    }
                });
            }
        } else if (type == "agent_status") {
            std::string status = msg.getString("status", "idle");
            std::string details = msg.getString("details", "");
            std::cerr << "[Amuse] Status: " << status << " - " << details << std::endl;
            setStatus(status, details);
            if (status == "idle" && details == "Ready" && getenv("AMUSE_EXIT_ON_READY")) {
                g_timeout_add(300, +[](gpointer) -> gboolean {
                    gtk_main_quit();
                    return G_SOURCE_REMOVE;
                }, nullptr);
            }
        } else if (type == "transcript_delta") {
            std::string role = msg.getString("role", "agent");
            std::string text = msg.getString("text", "");
            updateTranscriptDelta(role, text);
        } else if (type == "transcript") {
            std::string role = msg.getString("role", "agent");
            std::string text = msg.getString("text", "");
            std::cerr << "[Amuse] Transcript (" << role << "): " << text << std::endl;
            appendTranscript(role, text);
        } else if (type == "tool_call_request") {
            std::string callId = msg.getString("callId", "");
            std::string toolName = msg.getString("toolName", "");
            std::string argsJson = msg.getObjectJson("arguments", "{}");

            std::cerr << "[Amuse] Executing local tool: " << toolName << "(" << sanitizeArgsForLog(argsJson) << ")" << std::endl;
            setStatus("thinking", "Working with " + toolName + "...");

            std::thread([this, callId, toolName, argsJson]() {
                std::string resultJson = LocalConnectorsManager::instance().executeTool(toolName, argsJson);
                appendTranscript("tool", "Connected & executed " + toolName, toolName);
                // Output as an escaped JSON string (never raw-embedded): truncated or
                // malformed tool output must not break the IPC line protocol.
                std::string resp = "{\"type\":\"tool_call_response\",\"callId\":\"" +
                                   jsonEscape(callId) + "\",\"toolName\":\"" +
                                   jsonEscape(toolName) + "\",\"output\":\"" + jsonEscape(resultJson) + "\"}";
                std::cerr << "[Amuse] Tool response ready (" << resultJson.size() << " bytes)" << std::endl;
                sendToNode(resp);
            }).detach();
        }
    }
};

// ============================================================================
// Entry Point
// ============================================================================

int main(int argc, char **argv) {
    if (argc > 1) {
        std::string cmd = argv[1];
        if (cmd == "tools") {
            std::cout << LocalConnectorsManager::instance().getToolDeclarationsJson() << std::endl;
            return 0;
        } else if (cmd == "call" && argc > 2) {
            std::string toolName = argv[2];
            std::string argsJson = (argc > 3) ? argv[3] : "{}";
            std::cout << LocalConnectorsManager::instance().executeTool(toolName, argsJson) << std::endl;
            return 0;
        }
    }

    AmuseAppController::instance().run(argc, argv);
    return 0;
}
