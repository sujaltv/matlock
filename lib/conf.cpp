/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/types.h>

#include "../include/conf.hpp"
#include "../include/utils.hpp"

#ifndef ETC_CONFIG
    #define ETC_CONFIG "/etc/matlock.yaml"
#endif
#define CONFIG_NAME "matlock.yaml"
#define MAX_CONFIG_SIZE (64 * 1024)


namespace {

enum class ParseStatus { Ok, Missing, Unreadable };


std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}


std::string parse_value(const std::string& raw) {
    /* strip surrounding quotes, or an unquoted trailing " #comment" */
    std::string v = trim(raw);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
        size_t end = v.find(v.front(), 1);
        if (end != std::string::npos)
            return v.substr(1, end - 1);
    }
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i] == '#' && (v[i - 1] == ' ' || v[i - 1] == '\t'))
            return trim(v.substr(0, i));
    }
    if (!v.empty() && v[0] == '#')
        return "";
    return v;
}


bool valid_colour(const std::string& s) {
    if (s.size() != 7 || s[0] != '#')
        return false;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}


void warn(const char* path, int line, const char* msg, const std::string& what) {
    fprintf(stderr, "%s: %s:%d: %s: '%s'\n", NAME, path, line, msg, what.c_str());
}


void set_colour(std::string& into, const std::string& v, const char* path, int line) {
    if (valid_colour(v))
        into = v;
    else
        warn(path, line, "invalid colour (expected \"#RRGGBB\")", v);
}


void set_bool(bool& into, const std::string& v, const char* path, int line) {
    if (v == "true")
        into = true;
    else if (v == "false")
        into = false;
    else
        warn(path, line, "invalid boolean (expected true or false)", v);
}


void set_int(int& into, const std::string& v, long lo, long hi,
             const char* path, int line) {
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end != '\0') {
        warn(path, line, "invalid value (expected an integer)", v);
    } else if (n < lo || n > hi) {
        char msg[64];
        snprintf(msg, sizeof(msg), "value out of range (%ld..%ld)", lo, hi);
        warn(path, line, msg, v);
    } else {
        into = (int)n;
    }
}


void set_key(Config& c, bool in_fontcolour, const std::string& key,
             const std::string& v, const char* path, int line) {
    if (in_fontcolour) {
        if (key == "init")
            set_colour(c.fontcolour[States::INIT], v, path, line);
        else if (key == "input")
            set_colour(c.fontcolour[States::INPUT], v, path, line);
        else if (key == "failed")
            set_colour(c.fontcolour[States::FAILED], v, path, line);
        else
            warn(path, line, "unknown fontcolour key", key);
        return;
    }

    if (key == "background") {
        set_colour(c.background, v, path, line);
    } else if (key == "font_pattern") {
        if (!v.empty())
            c.font_pattern = v;
        else
            warn(path, line, "empty value", key);
    } else if (key == "font_size") {
        set_int(c.font_size, v, FARTHEST_FONT_SIZE, 128, path, line);
    } else if (key == "depth_levels") {
        set_int(c.depth_levels, v, 1, 8, path, line);
    } else if (key == "max_droplets") {
        set_int(c.max_droplets, v, 16, 20000, path, line);
    } else if (key == "droplet_length") {
        set_int(c.droplet_length, v, 2, 200, path, line);
    } else if (key == "spawn_attempts") {
        set_int(c.spawn_attempts, v, 1, 32, path, line);
    } else if (key == "charset") {
        std::string filtered;
        for (char ch : v)
            if (ch >= 0x21 && ch <= 0x7E)
                filtered += ch;
        if (filtered.empty() || filtered.size() > 256)
            warn(path, line, "invalid charset (1..256 printable ASCII characters)", v);
        else {
            if (filtered.size() != v.size())
                warn(path, line, "charset: ignored non-printable-ASCII characters", v);
            c.charset = filtered;
        }
    } else if (key == "mutate_chars") {
        set_bool(c.mutate_chars, v, path, line);
    } else if (key == "failonclear") {
        set_bool(c.failonclear, v, path, line);
    } else {
        warn(path, line, "unknown key", key);
    }
}


ParseStatus parse_file(const char* path, Config& c) {
    /**
     * Parse a strict, flat YAML subset: top-level "key: value" scalars plus
     * one "fontcolour:" mapping with indented "key: value" entries.
     * Anything else warns and is skipped; the locker never dies on config.
     */

    FILE* f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return ParseStatus::Missing;
        fprintf(stderr, "%s: cannot read %s: %s\n", NAME, path,
                strerror(errno));
        return ParseStatus::Unreadable;
    }

    std::string text(MAX_CONFIG_SIZE, '\0');
    size_t n = fread(text.data(), 1, MAX_CONFIG_SIZE, f);
    bool truncated = n == MAX_CONFIG_SIZE && !feof(f);
    fclose(f);
    text.resize(n);
    if (truncated)
        fprintf(stderr, "%s: %s: file too large, reading first %d bytes\n",
                NAME, path, MAX_CONFIG_SIZE);

    bool in_fontcolour = false;
    int lineno = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos
                                            ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() + 1 : eol + 1;
        lineno++;

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        std::string content = trim(line);
        if (content.empty() || content[0] == '#')
            continue;
        if (content == "---" || content == "...")   // document markers
            continue;
        if (!indented)
            in_fontcolour = false;

        size_t colon = content.find(':');
        if (colon == std::string::npos) {
            warn(path, lineno, "expected 'key: value'", content);
            continue;
        }
        std::string key = trim(content.substr(0, colon));
        std::string value = parse_value(content.substr(colon + 1));

        if (!indented && key == "fontcolour" && value.empty()) {
            in_fontcolour = true;
            continue;
        }
        if (indented && !in_fontcolour) {
            warn(path, lineno, "unexpected indented key", key);
            continue;
        }
        set_key(c, indented && in_fontcolour, key, value, path, lineno);
    }

    return ParseStatus::Ok;
}

} // namespace


Conf::Conf() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    if (xdg && *xdg)
        this->user_dir_ = std::string(xdg) + "/matlock";
    else if (home && *home)
        this->user_dir_ = std::string(home) + "/.config/matlock";
    if (!this->user_dir_.empty())
        this->user_path_ = this->user_dir_ + "/" CONFIG_NAME;

    this->load(this->cfg_, true);

    this->inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (this->inotify_fd_ < 0)
        return;
    const uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                          IN_DELETE | IN_MOVED_FROM;
    inotify_add_watch(this->inotify_fd_, "/etc", mask);
    if (!this->user_dir_.empty())
        inotify_add_watch(this->inotify_fd_, this->user_dir_.c_str(), mask);
}


Conf::~Conf() {
    if (this->inotify_fd_ >= 0)
        close(this->inotify_fd_);
}


bool Conf::load(Config& into, bool initial) {
    into = Config{};

    /* /etc/matlock.yaml is a fixed, root-owned path: safe to read as root. */
    ParseStatus etc = parse_file(ETC_CONFIG, into);

    /* The per-user file path derives from $XDG_CONFIG_HOME/$HOME, which the
     * invoking user controls. The initial load runs with euid 0 (this is a
     * suid binary), so a symlink there could make root open a file the user
     * cannot: read it as the real user instead, letting the kernel resolve
     * the whole path (symlinks, permissions) with the user's rights. The
     * saved-set-uid is still root here, so the euid is restored afterwards.
     * Hot reloads run after the permanent drop to 'nobody' (euid != 0), so
     * the swap is skipped and the file is read as 'nobody', as before. */
    ParseStatus user = ParseStatus::Missing;
    if (!this->user_path_.empty()) {
        uid_t ruid = getuid();
        bool swapped = false;
        if (geteuid() == 0 && ruid != 0) {
            if (seteuid(ruid) == 0) {
                swapped = true;
            } else {
                fprintf(stderr, "%s: refusing to read %s as root: %s\n", NAME,
                        this->user_path_.c_str(), strerror(errno));
                user = ParseStatus::Unreadable;
            }
        }
        if (user != ParseStatus::Unreadable)
            user = parse_file(this->user_path_.c_str(), into);
        if (swapped && seteuid(0) != 0)
            Utils::die("%s: could not restore privileges: %s\n", NAME,
                       strerror(errno));
    }

    /* on reload, refuse to apply a half-view of the configuration: keep the
     * current one until both files are readable again */
    if (!initial &&
        (etc == ParseStatus::Unreadable || user == ParseStatus::Unreadable))
        return false;
    return true;
}


bool Conf::reload_if_changed() {
    if (this->inotify_fd_ < 0)
        return false;

    bool relevant = false;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    while ((n = read(this->inotify_fd_, buf, sizeof(buf))) > 0) {
        for (char* p = buf; p < buf + n;) {
            auto* ev = (struct inotify_event*)p;
            if (ev->len && !strcmp(ev->name, CONFIG_NAME))
                relevant = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (!relevant)
        return false;

    Config nc;
    if (!this->load(nc, false))
        return false;
    if (nc == this->cfg_)
        return false;
    this->cfg_ = nc;
    return true;
}
