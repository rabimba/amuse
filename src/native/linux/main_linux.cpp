#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <sqlite3.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>

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
#include <sys/wait.h>
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

static void spawnDetachedProcess(const std::vector<std::string> &args) {
    if (args.empty()) return;
    std::thread([args]() {
        pid_t pid = fork();
        if (pid == 0) {
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
            waitpid(pid, &status, WNOHANG);
        }
    }).detach();
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
    "description": "Auto-connects to LinkedIn or social media platforms to draft, schedule, or publish a post. Copies post text to clipboard, saves draft, schedules reminder/alarm if schedule_time is provided, and can open the LinkedIn post composer in the browser.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "platform": {
          "type": "STRING",
          "description": "Social platform: 'linkedin' (default), 'twitter_x', 'facebook'"
        },
        "action": {
          "type": "STRING",
          "description": "Action: 'schedule', 'draft', or 'open_composer'"
        },
        "content": {
          "type": "STRING",
          "description": "Post text or topic content"
        },
        "schedule_time": {
          "type": "STRING",
          "description": "Optional time to schedule the post for (e.g. '09:00 CST' or ISO-8601)"
        },
        "open_in_browser": {
          "type": "BOOLEAN",
          "description": "If true, opens LinkedIn/platform post composer in the browser now"
        }
      },
      "required": ["content"]
    }
  },
  {
    "name": "email_action",
    "description": "Auto-connects to Gmail / Email connector to search emails, draft an email, or open a pre-filled compose window.",
    "parameters": {
      "type": "OBJECT",
      "properties": {
        "action": {
          "type": "STRING",
          "description": "'search', 'draft', or 'send'"
        },
        "to": {
          "type": "STRING",
          "description": "Recipient email address or name"
        },
        "subject": {
          "type": "STRING",
          "description": "Email subject line or search query"
        },
        "body": {
          "type": "STRING",
          "description": "Email body content"
        },
        "open_mail_client": {
          "type": "BOOLEAN",
          "description": "If true, opens Gmail compose in browser or desktop mail client"
        }
      },
      "required": ["action"]
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
  }
])JSON";
    }

    std::string executeTool(const std::string &toolName, const std::string &argsJson) {
        std::lock_guard<std::mutex> lock(dbMutex_);
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
            return socialMediaAction(
                platform,
                args.getString("action", "schedule"),
                args.getString("content", ""),
                args.getString("schedule_time", ""),
                args.getBool("open_in_browser", false)
            );
        } else if (toolName == "email_action") {
            autoConnect("gmail");
            return emailAction(
                args.getString("action", "search"),
                args.getString("to", ""),
                args.getString("subject", ""),
                args.getString("body", ""),
                args.getBool("open_mail_client", false)
            );
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
            return computerControlAction(
                args.getString("action", ""),
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
        }

        return "{\"error\":\"Unknown tool: " + jsonEscape(toolName) + "\"}";
    }

    void emitMemoryAndPlansToHUD() {
        std::lock_guard<std::mutex> lock(dbMutex_);
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
        std::lock_guard<std::mutex> lock(dbMutex_);
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
        std::lock_guard<std::mutex> lock(dbMutex_);
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
            "🔌 Active Muse Connectors (20 Services)",
            "Auto-Connect Enabled",
            "Local + Cloud Connectors in ~/.amuse/local_data.db",
            body.str(),
            getNotesDir(),
            "Open Amuse Notes Folder"
        });
    }

private:
    sqlite3 *db_ = nullptr;
    std::mutex dbMutex_;
    std::mutex cbMutex_;
    RichCardCallback richCardCb_;
    ImageFrameCallback imageFrameCb_;

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

    static std::string writeNoteMarkdownFile(const std::string &title, const std::string &dateStr, const std::string &body) {
        std::string dir = getNotesDir();
        std::string filename = sanitizeFilename(title) + ".md";
        std::string fullPath = dir + "/" + filename;
        std::ofstream ofs(fullPath, std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << "# " << title << "\n\n";
            ofs << "*Updated: " << dateStr << "*\n\n";
            ofs << body << "\n";
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
        if (tableRowCount("connectors") < 20) {
            const char *sql = R"(
                INSERT OR IGNORE INTO connectors (id, name, category, status, permission, description, last_used) VALUES
                ('notes', 'Local Markdown Notes (~/Documents/AmuseNotes)', 'local', 'connected', 'read_and_write', 'Reads and writes Markdown notes in ~/Documents/AmuseNotes/ and ~/Notes/ + HUD Note Viewer', 'Active'),
                ('calendar', 'Local & GNOME Calendar (.ics)', 'local', 'connected', 'read_and_write', 'Manages local calendar events and syncs with GNOME Evolution calendar.ics', 'Active'),
                ('reminders', 'Local & GNOME Tasks (.ics)', 'local', 'connected', 'read_and_write', 'Manages to-do reminders and syncs with GNOME Evolution tasks.ics', 'Active'),
                ('alarms_timers', 'System Alarms, Timers & Scheduler', 'local', 'connected', 'read_and_write', 'Background alarms, countdown timers, audio chimes, and scheduled tasks', 'Active'),
                ('messages', 'Local Messages & Chat', 'local', 'connected', 'read_and_write', 'Searches and sends local messages and chat logs', 'Active'),
                ('contacts', 'Local Contacts & Address Book', 'local', 'connected', 'read_and_write', 'Searches and manages contacts, phone numbers, and emails', 'Active'),
                ('files_disk', 'Full-Disk File Assistant', 'local', 'connected', 'read_and_write', 'Searches, reads, writes, and organizes files in ~/Documents, ~/Downloads, ~/Desktop', 'Active'),
                ('computer_control', 'Computer Control, Screenshot & Screencast', 'local', 'connected', 'read_and_write', 'Records ffmpeg MP4 screencasts, captures PNG screenshots, controls apps & clipboard', 'Active'),
                ('active_memory', 'Muse Active Memory & Action Plans', 'local', 'connected', 'read_and_write', 'Persists user preferences, facts, habits, and multi-step project plans across sessions', 'Active'),
                ('dynamic_tools', 'Muse Dynamic Custom Tool Builder', 'local', 'connected', 'read_and_write', 'Dynamically writes and executes custom Python and Bash tools in ~/.amuse/custom_tools/', 'Active'),
                ('web_browser', 'Live Web Search & Browser Automation', 'cloud', 'connected', 'read_and_write', 'Searches the live web, reads webpages, and opens browser workflows', 'Active'),
                ('linkedin', 'LinkedIn Connector', 'cloud', 'connected', 'read_and_write', 'Drafts, schedules, copies, and launches LinkedIn posts and share workflows', 'Active'),
                ('gmail', 'Gmail & Email Connector', 'cloud', 'connected', 'read_and_write', 'Searches, drafts, and composes emails via local DB and Gmail web/mail client', 'Active'),
                ('google_calendar', 'Google Calendar Connector', 'cloud', 'connected', 'read_and_write', 'Syncs events and opens Google Calendar scheduling workflows', 'Active'),
                ('google_docs', 'Google Docs & Drive Connector', 'cloud', 'connected', 'read_and_write', 'Creates and syncs documents locally and via Google Docs', 'Active'),
                ('slack', 'Slack Workspace Connector', 'cloud', 'connected', 'read_and_write', 'Sends and searches Slack workspace messages', 'Active'),
                ('notion', 'Notion Workspace Connector', 'cloud', 'connected', 'read_and_write', 'Syncs Notion pages and Markdown documents in ~/Documents/AmuseNotes/Notion', 'Active'),
                ('github', 'GitHub Connector (gh CLI & Web)', 'cloud', 'connected', 'read_and_write', 'Queries repositories, issues, and PRs via GitHub CLI and web', 'Active'),
                ('whatsapp', 'WhatsApp Connector', 'cloud', 'connected', 'read_and_write', 'Sends and tracks WhatsApp messages and opens WhatsApp Web', 'Active'),
                ('spotify', 'Spotify & System Media Connector', 'cloud', 'connected', 'read_and_write', 'Controls desktop media playback (MPRIS), volume, and Spotify', 'Active');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("memories") == 0) {
            const char *sql = R"(
                INSERT INTO memories (key, value, category, updated_at) VALUES
                ('user_name', 'Rabimba Karanjai', 'personal', '2026-10-13 05:00:00'),
                ('desktop_environment', 'Ubuntu 24.04 Linux Desktop (X11/GNOME, PipeWire audio). Notes are stored as Markdown files in ~/Documents/AmuseNotes/ and ~/Notes/.', 'preference', '2026-10-13 05:00:00'),
                ('active_project', 'Building Amuse with full Meta Muse feature parity, duplex Gemini Live voice, and local/cloud connectors.', 'project', '2026-10-13 05:00:00');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("action_plans") == 0) {
            const char *sql = R"(
                INSERT INTO action_plans (id, goal, steps, status, updated_at) VALUES
                ('plan-001', 'Amuse Launch & LinkedIn Demo', '1. [x] Port native HUD & PipeWire audio to Linux\n2. [x] Enable 20 Muse connectors & Markdown Notes\n3. [ ] Record 5s screencast & publish LinkedIn post at 9:00 AM CST', 'in_progress', '2026-10-13 05:10:00');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("emails") == 0) {
            const char *sql = R"(
                INSERT INTO emails (id, sender, recipient, subject, body, date, status) VALUES
                ('mail-001', 'alex.chen@example.com', 'rabimba@mozilla.com', 'Amuse Multimodal Live Architecture Feedback', 'The duplex audio latency on Linux PipeWire looks great. Let us review the Muse connectors list at 2 PM.', '2026-10-13 08:15:00', 'received'),
                ('mail-002', 'sarah.jenkins@example.com', 'rabimba@mozilla.com', 'LinkedIn Launch Announcement & Screencast Demo', 'Remember to post the Amuse demo screencast on LinkedIn this morning!', '2026-10-13 08:45:00', 'received');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("messages") == 0) {
            const char *sql = R"(
                INSERT INTO messages (guid, text, sender, is_from_me, date) VALUES
                ('msg-101', 'Hey! Did you get a chance to decompile the Endo Mach-O binary and inspect the Gemini Live bridge symbols?', '+14155550192 (Alex Chen)', 0, '2026-10-12 18:42:10'),
                ('msg-102', 'Yes, finished the Decompile pass on HatchClient and S2SAudioEngine. Pushing the architecture notes now.', 'Me', 1, '2026-10-12 18:45:02'),
                ('msg-103', 'Are we still on for the AI Agent architecture review today at 2:00 PM?', 'Sarah Jenkins', 0, '2026-10-13 09:15:00'),
                ('msg-104', 'Production GPU cluster health check passed: all 8 inference nodes online.', 'DevOps Bot', 0, '2026-10-13 10:30:00');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("notes") == 0) {
            const char *sql = R"(
                INSERT INTO notes (title, date, preview, body) VALUES
                ('Infrastructure & Servers Inventory', '2026-10-12 16:20:00',
                 'Primary GPU Servers: node-alpha (8x H100, 10.0.1.10), node-beta (4x A100, 10.0.1.11), staging-api (Ubuntu 24.04, port 8443).',
                 'Primary GPU Servers: node-alpha (8x H100, 10.0.1.10), node-beta (4x A100, 10.0.1.11), staging-api (Ubuntu 24.04, port 8443). NAS backup on 10.0.1.50.'),
                ('Amuse Agent Architecture & Roadmap', '2026-10-13 01:05:00',
                 '1. Gemini Multimodal Live duplex audio stream. 2. Local SQLite & desktop connectors. 3. Double-tap Option/Alt floating HUD.',
                 '1. Gemini Multimodal Live duplex audio stream. 2. Local SQLite & desktop connectors. 3. Double-tap Option/Alt floating HUD.'),
                ('Binary Reverse Engineering Notes', '2026-10-11 21:10:00',
                 'Decompile analysis of Endo binary: DoubleOptionTapShortcutHandler, S2SAudioEngine, LocalConnectorsManager.',
                 'Decompile analysis of Endo binary: DoubleOptionTapShortcutHandler, S2SAudioEngine, LocalConnectorsManager.');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("contacts") == 0) {
            const char *sql = R"(
                INSERT INTO contacts (id, given_name, family_name, full_name, organization, phone_label, phone_number, email_label, email_address) VALUES
                ('contact-001', 'Alex', 'Chen', 'Alex Chen', 'DeepMind Systems', 'Mobile', '+1-415-555-0192', 'Work', 'alex.chen@example.com'),
                ('contact-002', 'Sarah', 'Jenkins', 'Sarah Jenkins', 'Mozilla Research', 'Mobile', '+1-650-555-0148', 'Work', 'sarah.jenkins@example.com'),
                ('contact-003', 'Rabimba', 'Karanjai', 'Rabimba Karanjai', 'Amuse Project', 'Mobile', '+1-408-555-0177', 'Work', 'rabimba@mozilla.com'),
                ('contact-004', 'Test', 'Contact', 'Test Contact', 'Amuse QA', 'Work', '+1-800-555-0199', 'Work', 'test@amuse.local');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("calendar_events") == 0) {
            const char *sql = R"(
                INSERT INTO calendar_events (id, title, start_iso, end_iso, is_all_day, location, notes, calendar) VALUES
                ('evt-001', 'AI Agent Architecture Sync', '2026-10-13T14:00:00-07:00', '2026-10-13T15:00:00-07:00', 0, 'Meet / Room 4B', 'Review Gemini Live duplex audio latency and local tool calling with Sarah and Alex.', 'Work'),
                ('evt-002', 'GPU Cluster Maintenance Window', '2026-10-15T10:00:00-07:00', '2026-10-15T11:30:00-07:00', 0, 'Data Center / Remote', 'Kernel update on node-alpha and node-beta servers.', 'Ops');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }

        if (tableRowCount("reminders") == 0) {
            const char *sql = R"(
                INSERT INTO reminders (id, title, completed, due_date, notes, calendar) VALUES
                ('rem-001', 'Publish Amuse v1.0 release notes', 0, '2026-10-14T17:00:00-07:00', 'Include Linux and macOS build instructions', 'Tasks'),
                ('rem-002', 'Rotate staging server SSH keys', 0, '2026-10-16T12:00:00-07:00', 'Update keys on node-alpha and staging-api', 'Ops');
            )";
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        }
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

    std::string listConnectors(const std::string &category) {
        std::string sql = "SELECT id, name, category, status, permission, description, last_used FROM connectors ";
        if (!category.empty() && category != "all") {
            sql += "WHERE category = '" + jsonEscape(category) + "' ";
        }
        sql += "ORDER BY category ASC, name ASC;";
        sqlite3_stmt *stmt = nullptr;
        std::ostringstream out;
        out << "{\"connectors\":[";
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
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
        out << "]}";
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
        parseTargetTime(targetTimeStr, durationSec, targetEpoch, secondsUntil, formattedTarget);
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
                std::lock_guard<std::mutex> lk(dbMutex_);
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

        std::string filePath = writeNoteMarkdownFile(title, dateStr, body);
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
                                  bool openInBrowser) {
        std::string id = "post-" + std::to_string(std::time(nullptr));
        std::string nowStr = currentReadableTimestamp();
        std::string status = scheduleTime.empty() ? "ready" : "scheduled";

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

        // Save as Markdown note draft as well & copy to clipboard
        std::string draftPath = writeNoteMarkdownFile("LinkedIn_Post_Draft", nowStr, content);
        copyToClipboard(content);

        std::string shareUrl = "https://www.linkedin.com/feed/?shareActive=true&text=" + urlEncode(content);
        if (platform == "twitter_x") {
            shareUrl = "https://twitter.com/intent/tweet?text=" + urlEncode(content);
        }

        std::string scheduledTarget;
        int secondsUntil = 0;
        if (!scheduleTime.empty()) {
            time_t targetEpoch = 0;
            parseTargetTime(scheduleTime, 0, targetEpoch, secondsUntil, scheduledTarget);
            setAlarmOrTimer("Publish " + platform + " Post", scheduleTime, 0);
            createCalendarEvent("Publish " + platform + " Post", scheduledTarget, "", shareUrl, content);
        }

        if (openInBrowser || action == "open_composer") {
            openOnDesktop(shareUrl);
        }

        sendDesktopNotification(
            "🚀 " + platform + " Post " + (scheduleTime.empty() ? "Prepared" : "Scheduled"),
            scheduleTime.empty() ? "Copied to clipboard & saved draft" : ("Scheduled for " + scheduledTarget)
        );

        emitRichCard({
            "connector",
            "🚀 " + platform + " Post (" + status + ")",
            platform,
            scheduleTime.empty() ? ("Saved to " + draftPath) : ("Scheduled for " + scheduledTarget),
            content,
            shareUrl,
            "Open LinkedIn Composer"
        });

        std::ostringstream out;
        out << "{"
            << "\"success\":true,"
            << "\"post_id\":\"" << jsonEscape(id) << "\","
            << "\"platform\":\"" << jsonEscape(platform) << "\","
            << "\"status\":\"" << jsonEscape(status) << "\","
            << "\"scheduled_for\":\"" << jsonEscape(scheduledTarget) << "\","
            << "\"seconds_until_scheduled\":" << secondsUntil << ","
            << "\"copied_to_clipboard\":true,"
            << "\"draft_file_path\":\"" << jsonEscape(draftPath) << "\","
            << "\"composer_url\":\"" << jsonEscape(shareUrl) << "\""
            << "}";
        return out.str();
    }

    std::string emailAction(const std::string &action, const std::string &to,
                            const std::string &subject, const std::string &body,
                            bool openMailClient) {
        if (action == "search") {
            std::string sql = "SELECT id, sender, recipient, subject, body, date, status FROM emails ";
            if (!subject.empty() || !to.empty()) {
                sql += "WHERE subject LIKE ? COLLATE NOCASE OR body LIKE ? COLLATE NOCASE OR sender LIKE ? COLLATE NOCASE ";
            }
            sql += "ORDER BY id DESC LIMIT 15;";
            sqlite3_stmt *stmt = nullptr;
            std::ostringstream out;
            out << "{\"emails\":[";
            bool first = true;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                if (!subject.empty() || !to.empty()) {
                    std::string q = "%" + (subject.empty() ? to : subject) + "%";
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
                        << "\"id\":\"" << jsonEscape(col(0)) << "\","
                        << "\"sender\":\"" << jsonEscape(col(1)) << "\","
                        << "\"recipient\":\"" << jsonEscape(col(2)) << "\","
                        << "\"subject\":\"" << jsonEscape(col(3)) << "\","
                        << "\"body\":\"" << jsonEscape(col(4)) << "\","
                        << "\"date\":\"" << jsonEscape(col(5)) << "\","
                        << "\"status\":\"" << jsonEscape(col(6)) << "\""
                        << "}";
                }
                sqlite3_finalize(stmt);
            }
            out << "]}";
            return out.str();
        }

        std::string id = "mail-" + std::to_string(std::time(nullptr));
        std::string nowStr = currentReadableTimestamp();
        const char *sql =
            "INSERT INTO emails (id, sender, recipient, subject, body, date, status) "
            "VALUES (?, 'rabimba@mozilla.com', ?, ?, ?, ?, ?);";
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
        if (openMailClient) {
            openOnDesktop(gmailUrl);
        }

        sendDesktopNotification("✉️ Email " + action, "To: " + to + " | Subject: " + subject);
        emitRichCard({
            "connector",
            "✉️ Email: " + subject,
            action,
            "To: " + to,
            body,
            gmailUrl,
            "Open in Gmail"
        });

        return "{\"success\":true,\"email_id\":\"" + jsonEscape(id) + "\",\"to\":\"" + jsonEscape(to) +
               "\",\"subject\":\"" + jsonEscape(subject) + "\",\"gmail_compose_url\":\"" + jsonEscape(gmailUrl) + "\"}";
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
            // Read PNG and stream into Gemini Multimodal Live Vision!
            std::ifstream ifs(filePath, std::ios::binary);
            if (ifs.is_open()) {
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                if (!bytes.empty()) {
                    emitImageFrame(base64Encode(bytes.data(), bytes.size()), "image/png");
                }
            }
            sendDesktopNotification("📸 Screenshot & Vision Shared", "Saved to " + filePath + " & streamed to Gemini Vision");
            emitRichCard({
                "media",
                "📸 Screen Vision Shared: " + title,
                "LIVE VISION",
                filePath,
                "Captured desktop screen (" + std::to_string(st.st_size / 1024) + " KB) and streamed to Gemini Multimodal Live.",
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
            << "\"streamed_to_gemini_vision\":true,"
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
            std::string cmd = "curl -sL -m 8 " + shellSingleQuote(queryOrUrl) +
                              " | sed -e 's/<script[^>]*>.*<\\/script>//gI' -e 's/<style[^>]*>.*<\\/style>//gI' -e 's/<[^>]*>/ /g' | tr -s ' \\n\\t' ' ' | head -c 4000";
            std::string text = runCommandCapture(cmd, 4096);
            return "{\"url\":\"" + jsonEscape(queryOrUrl) + "\",\"content_excerpt\":\"" + jsonEscape(text) + "\"}";
        }
        std::string ddgUrl = "https://html.duckduckgo.com/html/?q=" + urlEncode(queryOrUrl);
        std::string cmd = "curl -sL -m 8 -A 'Mozilla/5.0' " + shellSingleQuote(ddgUrl) +
                          " | grep -oP '(?<=class=\"result__snippet\"[^>]*>).*?(?=</a>|</td>|</div>)' 2>/dev/null | sed 's/<[^>]*>//g' | head -n 8";
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
        parseTargetTime(targetTimeStr, 0, targetEpoch, secondsUntil, formattedTarget);
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
                std::lock_guard<std::mutex> lk(dbMutex_);
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
                std::lock_guard<std::mutex> lk(dbMutex_);
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
        std::string memPath = writeNoteMarkdownFile("Amuse_Active_Memory", nowStr, md.str());

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
            std::string planPath = writeNoteMarkdownFile("Action_Plan_" + goal, nowStr, steps);
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

        std::string runner = isPy ? "/usr/bin/python3 " : "/bin/bash ";
        std::string output = runCommandCapture(runner + shellSingleQuote(scriptPath) + " 2>&1", 8192);

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
            std::thread([]() {
                LocalConnectorsManager::instance().executeTool("take_screenshot", "{\"title\":\"Live_Screen_Vision\"}");
            }).detach();
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
        gtk_window_set_default_size(GTK_WINDOW(window_), 480, 620);
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
        std::string scriptPath = cwd + "/src/js/index.mjs";

        pid_t pid = fork();
        if (pid == 0) {
            dup2(stdinPipe[0], STDIN_FILENO);
            dup2(stdoutPipe[1], STDOUT_FILENO);
            close(stdinPipe[0]);
            close(stdinPipe[1]);
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);

            execlp("node", "node", scriptPath.c_str(), nullptr);
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
        } else if (type == "transcript") {
            std::string role = msg.getString("role", "agent");
            std::string text = msg.getString("text", "");
            std::cerr << "[Amuse] Transcript (" << role << "): " << text << std::endl;
            appendTranscript(role, text);
        } else if (type == "tool_call_request") {
            std::string callId = msg.getString("callId", "");
            std::string toolName = msg.getString("toolName", "");
            std::string argsJson = msg.getObjectJson("arguments", "{}");

            std::cerr << "[Amuse] Executing local tool: " << toolName << "(" << argsJson << ")" << std::endl;
            setStatus("thinking", "Accessing " + toolName + "...");

            std::thread([this, callId, toolName, argsJson]() {
                std::string resultJson = LocalConnectorsManager::instance().executeTool(toolName, argsJson);
                appendTranscript("tool", "Connected & executed " + toolName, toolName);
                std::string resp = "{\"type\":\"tool_call_response\",\"callId\":\"" +
                                   jsonEscape(callId) + "\",\"toolName\":\"" +
                                   jsonEscape(toolName) + "\",\"output\":" + resultJson + "}";
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
