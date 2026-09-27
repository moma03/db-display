// dotenv.h
// Minimal .env reader, so database credentials live in a file whose name says
// it holds secrets.
//
// A ".env" is a convention people recognise: it is gitignored by default
// almost everywhere, backup and deploy tooling treats it as sensitive, and
// anyone who sees one knows to protect it. A "config.json" carries none of
// that signal, so credentials sitting in one tend to get committed, pasted
// into issues, or copied to a shared host by someone who assumed it was just
// settings. Keeping them apart is worth the extra file.
//
// Format matches the one already used by timetable-fetcher:
//
//     # comment
//     DB_PG_HOST=10.0.10.132
//     DB_PG_PASSWORD="quoted if it contains spaces or #"
//     export DB_PG_USER=also_fine
//
// Values may be bare, single- or double-quoted. In a double-quoted value \n,
// \t, \" and \\ are unescaped; single-quoted values are literal. An unquoted
// value ends at a ' #' comment, which is why a password containing '#' must
// be quoted.

#ifndef LED_RENDERER_DOTENV_H
#define LED_RENDERER_DOTENV_H

#include <map>
#include <string>

namespace led {

using EnvMap = std::map<std::string, std::string>;

// Parse `path`. Returns false only if the file exists but cannot be read;
// a missing file is not an error (credentials may come from the real
// environment instead, e.g. systemd's EnvironmentFile=).
bool LoadDotEnv(const std::string &path, EnvMap *out, bool *existed,
                std::string *err);

// Look up `key`, preferring the real process environment over the .env file so
// a one-off `DB_PG_HOST=... ./led_renderer` still works and systemd's
// EnvironmentFile= keeps its usual precedence. Returns false when unset or
// empty in both.
bool EnvLookup(const EnvMap &env, const char *key, std::string *out);

}  // namespace led

#endif  // LED_RENDERER_DOTENV_H
