#include "dotenv.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace led {

namespace {

void Trim(std::string *s) {
    const char *ws = " \t\r\n";
    const size_t b = s->find_first_not_of(ws);
    if (b == std::string::npos) { s->clear(); return; }
    const size_t e = s->find_last_not_of(ws);
    *s = s->substr(b, e - b + 1);
}

std::string Unescape(const std::string &v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 >= v.size()) { out += v[i]; continue; }
        switch (v[++i]) {
        case 'n':  out += '\n'; break;
        case 't':  out += '\t'; break;
        case 'r':  out += '\r'; break;
        case '"':  out += '"';  break;
        case '\'': out += '\''; break;
        case '\\': out += '\\'; break;
        default:   out += '\\'; out += v[i]; break;
        }
    }
    return out;
}

}  // namespace

bool LoadDotEnv(const std::string &path, EnvMap *out, bool *existed,
                std::string *err) {
    *existed = false;
    std::ifstream f(path);
    if (!f.is_open()) return true;   // absent is fine
    *existed = true;

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        Trim(&line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("export ", 0) == 0) line = line.substr(7);

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            *err = path + ":" + std::to_string(lineno) + ": expected KEY=VALUE";
            return false;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        Trim(&key);
        Trim(&val);
        if (key.empty()) {
            *err = path + ":" + std::to_string(lineno) + ": empty key";
            return false;
        }

        if (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
            // Quoted. Scan for the matching close quote rather than assuming it
            // is the last character on the line -- a trailing comment may
            // follow it, and a '#' INSIDE the quotes must not end the value.
            const char q = val.front();
            std::string body;
            size_t i = 1;
            bool closed = false;
            for (; i < val.size(); ++i) {
                if (q == '"' && val[i] == '\\' && i + 1 < val.size()) {
                    body += val[i];
                    body += val[i + 1];
                    ++i;
                    continue;
                }
                if (val[i] == q) { closed = true; ++i; break; }
                body += val[i];
            }
            if (!closed) {
                *err = path + ":" + std::to_string(lineno) + ": unterminated " +
                       (q == '"' ? "double" : "single") + " quote";
                return false;
            }
            // Only whitespace or a comment may follow the closing quote.
            std::string rest = val.substr(i);
            Trim(&rest);
            if (!rest.empty() && rest[0] != '#') {
                *err = path + ":" + std::to_string(lineno) +
                       ": unexpected text after closing quote";
                return false;
            }
            val = (q == '"') ? Unescape(body) : body;   // '...' is literal
        } else {
            // Unquoted: a ' #' starts a trailing comment. This is why a value
            // containing '#' has to be quoted.
            const size_t c = val.find(" #");
            if (c != std::string::npos) val = val.substr(0, c);
            Trim(&val);
        }
        (*out)[key] = val;
    }
    return true;
}

bool EnvLookup(const EnvMap &env, const char *key, std::string *out) {
    if (const char *v = std::getenv(key)) {
        if (*v) { *out = v; return true; }
    }
    auto it = env.find(key);
    if (it != env.end() && !it->second.empty()) { *out = it->second; return true; }
    return false;
}

}  // namespace led
