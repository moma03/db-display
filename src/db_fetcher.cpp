// db_fetcher.cpp
// PostgreSQL data fetcher for the Deutsche Bahn display.
// Uses libpq to query planned_events + changed_events and build
// FetchedDeparture structs for the renderer.

#include "db_fetcher.h"
#include "delay_codes.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <libpq-fe.h>
#include <json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Replace every occurrence of `from` with `to` in `str`.
static void replaceAll(std::string &str, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Convert a pipe-separated path ("Köln Hbf|Düsseldorf Hbf|Duisburg Hbf")
// to comma-separated display string ("Köln Hbf, Düsseldorf Hbf, Duisburg Hbf").
static std::string pathToStops(const std::string &pipePath) {
    std::string out = pipePath;
    replaceAll(out, "|", ", ");
    return out;
}

// Format a PostgreSQL timestamp string to "HH:MM".
// Accepts formats like "2025-03-05 12:38:00" or ISO "2025-03-05T12:38:00".
static std::string formatTime(const char *ts) {
    if (!ts || !*ts) return "";
    std::string s(ts);
    // Find the time portion (after ' ' or 'T')
    auto tpos = s.find('T');
    if (tpos == std::string::npos) tpos = s.find(' ');
    if (tpos == std::string::npos) return "";
    // Extract HH:MM
    std::string timePart = s.substr(tpos + 1);
    if (timePart.size() >= 5)
        return timePart.substr(0, 5);  // "HH:MM"
    return timePart;
}

// Safe helper: return field value or empty string if NULL.
static std::string getField(PGresult *res, int row, int col) {
    if (PQgetisnull(res, row, col)) return "";
    const char *v = PQgetvalue(res, row, col);
    return v ? std::string(v) : "";
}

// ---------------------------------------------------------------------------
// Config loader
// ---------------------------------------------------------------------------

bool LoadDBConnectionConfig(const std::string &json_path, DBConnectionConfig &cfg) {
    std::ifstream f(json_path);
    if (!f.is_open()) return false;
    try {
        json j = json::parse(f);
        if (!j.contains("database")) return false;
        auto &db = j["database"];
        if (db.contains("host"))     cfg.host     = db["host"].get<std::string>();
        if (db.contains("port"))     cfg.port     = std::to_string(db["port"].get<int>());
        if (db.contains("dbname"))   cfg.dbname   = db["dbname"].get<std::string>();
        if (db.contains("user"))     cfg.user     = db["user"].get<std::string>();
        if (db.contains("password")) cfg.password = db["password"].get<std::string>();
        if (db.contains("eva"))      cfg.eva      = db["eva"].get<int>();
    } catch (const std::exception &e) {
        std::cerr << "db_fetcher config error: " << e.what() << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Main query
// ---------------------------------------------------------------------------

int FetchDepartures(const DBConnectionConfig &cfg,
                    FetchedStationInfo &station_out,
                    std::vector<FetchedDeparture> &out,
                    std::string &ticker_out)
{
    out.clear();
    ticker_out.clear();

    // Build connection string
    std::ostringstream connStr;
    connStr << "host=" << cfg.host
            << " port=" << cfg.port
            << " dbname=" << cfg.dbname
            << " user=" << cfg.user
            << " password='" << cfg.password << "'"
            << " options='-c timezone=Europe/Berlin'";

    PGconn *conn = PQconnectdb(connStr.str().c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB connection failed: " << PQerrorMessage(conn) << std::endl;
        PQfinish(conn);
        return -1;
    }

    // --- 1. Fetch station name ---
    {
        std::string q = "SELECT name FROM stations WHERE eva = " + std::to_string(cfg.eva) + " LIMIT 1";
        PGresult *res = PQexec(conn, q.c_str());
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            station_out.name = getField(res, 0, 0);
            station_out.eva  = cfg.eva;
        } else {
            station_out.name = "Station " + std::to_string(cfg.eva);
            station_out.eva  = cfg.eva;
        }
        PQclear(res);
    }

    // --- 2. Fetch upcoming departures ---
    // TODO: Refactor this query — the LATERAL + json_agg subquery is functional
    //       but ugly. Consider a cleaner approach (e.g. a DB view or two-step fetch).
    // Join planned_events with latest changed_events per stop_id.
    // Only departures (event_type = 'dep'), not hidden, from now - 5 min onward.
    // Order by effective departure time (changed_time if available, else planned_time).
    // Use LATERAL subquery to pick only the most recent changed_event
    // per stop_id (the history table can have many rows per train).
    const char *departureQuery =
        "SELECT "
        "  p.stop_id, "
        "  p.planned_platform, "
        "  p.planned_line, "
        "  p.planned_destination, "
        "  p.planned_time, "
        "  p.planned_path, "
        "  p.category, "
        "  p.train_number, "
        "  c.changed_time, "
        "  c.changed_platform, "
        "  c.changed_status, "
        "  c.changed_line, "
        "  c.changed_destination, "
        "  c.messages "
        "FROM planned_events p "
        "LEFT JOIN LATERAL ( "
        "  SELECT ce.changed_time, ce.changed_platform, ce.changed_status, "
        "         ce.changed_line, ce.changed_destination, "
        "         ( SELECT json_agg(json_build_object("
        "               'c', em.code, 't', em.message_type"
        "           )) "
        "           FROM changed_event_messages cem "
        "           JOIN event_messages em ON em.id = cem.event_message_id "
        "           WHERE cem.changed_event_id = ce.id "
        "         ) AS messages "
        "  FROM changed_events ce "
        "  WHERE ce.stop_id = p.stop_id "
        "    AND ce.event_type = p.event_type "
        "  ORDER BY ce.fetched_at DESC "
        "  LIMIT 1 "
        ") c ON true "
        "WHERE p.eva = $1 "
        "  AND p.event_type = 'dep' "
        "  AND p.planned_time >= (NOW() - INTERVAL '5 minutes') "
        "  AND (p.hidden IS NULL OR p.hidden = false) "
        "ORDER BY COALESCE(c.changed_time, p.planned_time) ASC "
        "LIMIT 50";

    // We need a stable pointer for paramValues
    std::string evaParam = std::to_string(cfg.eva);
    const char *paramValues[1] = { evaParam.c_str() };

    PGresult *res = PQexecParams(conn, departureQuery,
                                  1,          // nParams
                                  nullptr,    // paramTypes (let PG infer)
                                  paramValues,
                                  nullptr,    // paramLengths
                                  nullptr,    // paramFormats
                                  0);         // text result format

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "Departure query failed: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        PQfinish(conn);
        return -1;
    }

    int nRows = PQntuples(res);

    // Column indices
    int cPPlat    = PQfnumber(res, "planned_platform");
    int cPLine    = PQfnumber(res, "planned_line");
    int cPDest    = PQfnumber(res, "planned_destination");
    int cPTime    = PQfnumber(res, "planned_time");
    int cPPath    = PQfnumber(res, "planned_path");
    int cCat      = PQfnumber(res, "category");
    int cTNum     = PQfnumber(res, "train_number");
    int cCTime    = PQfnumber(res, "changed_time");
    int cCPlat    = PQfnumber(res, "changed_platform");
    int cCStat    = PQfnumber(res, "changed_status");
    int cCLine    = PQfnumber(res, "changed_line");
    int cCDest    = PQfnumber(res, "changed_destination");
    int cMsgs     = PQfnumber(res, "messages");

    for (int r = 0; r < nRows; ++r) {
        FetchedDeparture dep;

        // Platform
        dep.platform = getField(res, r, cPPlat);
        std::string cp = getField(res, r, cCPlat);
        if (!cp.empty() && cp != dep.platform)
            dep.cPlatform = cp;

        // Line: prefer planned_line, fall back to "category train_number"
        dep.line = getField(res, r, cPLine);
        if (dep.line.empty()) {
            std::string cat = getField(res, r, cCat);
            std::string num = getField(res, r, cTNum);
            if (!cat.empty() && !num.empty())
                dep.line = cat + " " + num;
            else if (!cat.empty())
                dep.line = cat;
        }
        // If a changed line exists and differs, we could show it, but
        // typically the line name doesn't change. Use changed_line as override:
        std::string cl = getField(res, r, cCLine);
        if (!cl.empty()) dep.line = cl;

        // Destination: prefer planned, override with changed if present
        dep.dest = getField(res, r, cPDest);
        std::string cd = getField(res, r, cCDest);
        if (!cd.empty()) dep.dest = cd;
        // Fallback: last entry of planned_path
        if (dep.dest.empty()) {
            std::string path = getField(res, r, cPPath);
            if (!path.empty()) {
                auto lastPipe = path.rfind('|');
                dep.dest = (lastPipe != std::string::npos)
                           ? path.substr(lastPipe + 1)
                           : path;
            }
        }

        // Times
        dep.pTime = formatTime(PQgetvalue(res, r, cPTime));
        std::string ctStr = getField(res, r, cCTime);
        if (!ctStr.empty()) {
            std::string ct = formatTime(ctStr.c_str());
            // Only show changed time if it differs from planned
            if (ct != dep.pTime)
                dep.cTime = ct;
        }

        // Intermediate stops from planned path
        std::string path = getField(res, r, cPPath);
        if (!path.empty())
            dep.stops = pathToStops(path);

        // Cancelled status
        std::string status = getField(res, r, cCStat);
        if (status == "c")
            dep.cancelled = true;

        // Decode delay/quality reason messages from JSON column
        std::string msgsJson = getField(res, r, cMsgs);
        if (!msgsJson.empty()) {
            try {
                auto msgs = json::parse(msgsJson);
                if (msgs.is_array()) {
                    for (auto &m : msgs) {
                        if (!m.is_object()) continue;
                        int code = -1;
                        std::string mType;
                        if (m.contains("c")) {
                            if (m["c"].is_number())
                                code = m["c"].get<int>();
                            else if (m["c"].is_string()) {
                                try { code = std::stoi(m["c"].get<std::string>()); }
                                catch (...) {}
                            }
                        }
                        if (m.contains("t") && m["t"].is_string())
                            mType = m["t"].get<std::string>();

                        // Only show delay (d) and quality (q) reasons
                        if ((mType == "d" || mType == "q") && code >= 0) {
                            const std::string &text = DelayReasonText(code);
                            if (!text.empty()) {
                                // Avoid duplicate notes
                                bool dup = false;
                                for (auto &existing : dep.notes)
                                    if (existing.id == code) { dup = true; break; }
                                if (!dup)
                                    dep.notes.push_back({code, text});
                            }
                        }
                    }
                }
            } catch (...) {
                // Malformed JSON — skip silently
            }
        }

        // Fallback note when delayed but no specific reason decoded
        if (dep.notes.empty() && !status.empty() && status != "p") {
            FetchedNote note;
            note.id = -1;
            if (dep.cancelled)
                note.text = "Zug fällt aus";
            else if (!dep.cTime.empty())
                note.text = "Verspätung";
            if (!note.text.empty())
                dep.notes.push_back(note);
        }

        out.push_back(dep);
    }

    PQclear(res);

    // --- 3. Fetch currently-valid station messages for ticker ---
    {
        const char *msgQuery =
            "SELECT text FROM station_messages "
            "WHERE eva = $1 "
            "  AND (deleted IS NULL OR deleted = false) "
            "  AND (valid_from IS NULL OR valid_from <= NOW()) "
            "  AND (valid_to   IS NULL OR valid_to   >= NOW()) "
            "  AND text IS NOT NULL AND text != '' "
            "ORDER BY timestamp DESC";

        PGresult *msgRes = PQexecParams(conn, msgQuery,
                                        1, nullptr, paramValues,
                                        nullptr, nullptr, 0);
        if (PQresultStatus(msgRes) == PGRES_TUPLES_OK) {
            int nMsgs = PQntuples(msgRes);
            std::ostringstream tickerBuf;
            for (int i = 0; i < nMsgs; ++i) {
                std::string txt = getField(msgRes, i, 0);
                if (!txt.empty()) {
                    if (tickerBuf.tellp() > 0)
                        tickerBuf << "  +++  ";
                    tickerBuf << txt;
                }
            }
            ticker_out = tickerBuf.str();
        }
        PQclear(msgRes);
    }

    PQfinish(conn);
    return (int)out.size();
}
