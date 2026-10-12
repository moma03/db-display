// db_live.cpp -- see db_live.h.

#include "db_live.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <libpq-fe.h>

namespace {

// Keep in sync with timetable-fetcher/src/db_manager.py.
constexpr const char *kUpdateChannel       = "timetable_update";
constexpr const char *kSubscriptionChannel = "display_subscriptions";

constexpr int kMinBackoffS = 2;
constexpr int kMaxBackoffS = 60;

bool IsMissingTable(PGresult *res) {
    const char *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    return state && std::strcmp(state, "42P01") == 0;   // undefined_table
}

}  // namespace

std::string DefaultDisplayId(const std::string &shm_path) {
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0 || !host[0])
        std::strcpy(host, "unknown-host");
    return std::string(host) + ":" + shm_path;
}

DbLiveSession::DbLiveSession(const DBConnectionConfig &cfg, std::string display_id)
    : cfg_(cfg), display_id_(std::move(display_id)), eva_str_(std::to_string(cfg.eva)) {}

DbLiveSession::~DbLiveSession() {
    Unsubscribe();
    if (conn_) PQfinish(conn_);
}

bool DbLiveSession::EnsureConnected(bool *fresh) {
    *fresh = false;
    if (conn_ && PQstatus(conn_) == CONNECTION_OK) return true;
    if (conn_) Drop("connection lost");

    const auto now = std::chrono::steady_clock::now();
    if (now < next_attempt_) return false;
    if (!Connect()) {
        backoff_s_ = backoff_s_ ? std::min(backoff_s_ * 2, kMaxBackoffS) : kMinBackoffS;
        next_attempt_ = now + std::chrono::seconds(backoff_s_);
        return false;
    }
    backoff_s_ = 0;
    *fresh = true;
    return true;
}

bool DbLiveSession::Connect() {
    // Keyword/value form, so a password with quotes or backslashes needs no
    // escaping. The keepalives make an idle LISTEN connection notice a dead
    // server or a dropped Wi-Fi link within about a minute instead of never.
    // Times in the database are naive Berlin local time, hence the timezone.
    const char *keys[] = {"host", "port", "dbname", "user", "password",
                          "connect_timeout", "application_name", "options",
                          "keepalives", "keepalives_idle", "keepalives_interval",
                          "keepalives_count", nullptr};
    const char *vals[] = {cfg_.host.c_str(), cfg_.port.c_str(), cfg_.dbname.c_str(),
                          cfg_.user.c_str(), cfg_.password.c_str(),
                          "10", "led_renderer", "-c timezone=Europe/Berlin",
                          "1", "30", "10", "3", nullptr};
    conn_ = PQconnectdbParams(keys, vals, 0);
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::fprintf(stderr, "renderer: DB connection failed: %s", PQerrorMessage(conn_));
        PQfinish(conn_);
        conn_ = nullptr;
        return false;
    }

    std::string listen = std::string("LISTEN ") + kUpdateChannel;
    PGresult *res = PQexec(conn_, listen.c_str());
    const bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok) std::fprintf(stderr, "renderer: LISTEN failed: %s", PQresultErrorMessage(res));
    PQclear(res);
    if (!ok) { Drop("LISTEN failed"); return false; }

    std::fprintf(stderr, "renderer: DB connected, listening on '%s', display id '%s'\n",
                 kUpdateChannel, display_id_.c_str());
    // Announce, so the fetcher picks a newly shown station up at once instead
    // of on its next periodic scan.
    if (!Subscribe(/*announce=*/true)) { Drop("subscribe failed"); return false; }
    return true;
}

void DbLiveSession::Drop(const char *why) {
    if (!conn_) return;
    std::fprintf(stderr, "renderer: DB session dropped (%s)\n", why);
    PQfinish(conn_);
    conn_ = nullptr;
}

bool DbLiveSession::Subscribe(bool announce) {
    if (!conn_) return false;
    if (!subscriptions_ok_) return true;

    // The upsert and the NOTIFY run as one statement, so the fetcher can never
    // see the notification before the row.
    const char *upsert =
        "INSERT INTO display_subscriptions (display_id, eva, created_at, last_seen) "
        "VALUES ($1, $2::int, now(), now()) "
        "ON CONFLICT (display_id) DO UPDATE SET eva = EXCLUDED.eva, last_seen = now()";
    const std::string announce_q =
        std::string("WITH up AS (") + upsert + " RETURNING eva) "
        "SELECT pg_notify('" + kSubscriptionChannel + "', eva::text) FROM up";

    const char *params[2] = {display_id_.c_str(), eva_str_.c_str()};
    PGresult *res = PQexecParams(conn_, announce ? announce_q.c_str() : upsert,
                                 2, nullptr, params, nullptr, nullptr, 0);
    const ExecStatusType st = PQresultStatus(res);
    bool ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
    if (!ok) {
        if (IsMissingTable(res)) {
            std::fprintf(stderr,
                "renderer: table display_subscriptions does not exist -- the "
                "timetable-fetcher predates subscriptions. Continuing without "
                "subscribing; updates arrive by polling only.\n");
            subscriptions_ok_ = false;
            ok = true;
        } else {
            std::fprintf(stderr, "renderer: subscription heartbeat failed: %s",
                         PQresultErrorMessage(res));
            // A failed statement on a live connection is not worth a
            // reconnect; only a dead connection is.
            ok = PQstatus(conn_) == CONNECTION_OK;
        }
    }
    PQclear(res);
    return ok;
}

bool DbLiveSession::Heartbeat() {
    if (!conn_) return false;
    if (Subscribe(/*announce=*/false)) return true;
    Drop("heartbeat failed");
    return false;
}

void DbLiveSession::Unsubscribe() {
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK || !subscriptions_ok_) return;
    const std::string q =
        std::string("WITH del AS (DELETE FROM display_subscriptions WHERE display_id = $1 "
                    "RETURNING eva) SELECT pg_notify('") + kSubscriptionChannel +
        "', eva::text) FROM del";
    const char *params[1] = {display_id_.c_str()};
    PGresult *res = PQexecParams(conn_, q.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
        std::fprintf(stderr, "renderer: unsubscribe failed: %s", PQresultErrorMessage(res));
    PQclear(res);
}

bool DbLiveSession::DrainNotifies() {
    bool ours = false;
    while (PGnotify *n = PQnotifies(conn_)) {
        // An empty payload means "everything changed"; otherwise only react
        // to our own station.
        if (!n->extra || !n->extra[0] || eva_str_ == n->extra) ours = true;
        PQfreemem(n);
    }
    return ours;
}

DbLiveSession::Wait DbLiveSession::WaitForUpdate(int timeout_ms) {
    if (!conn_) return Wait::kLost;

    // Notifications that came in while a query was running are already
    // buffered; report them before blocking.
    if (DrainNotifies()) return Wait::kNotified;

    const int sock = PQsocket(conn_);
    if (sock < 0) { Drop("no socket"); return Wait::kLost; }

    pollfd p{sock, POLLIN, 0};
    const int r = poll(&p, 1, std::max(0, timeout_ms));
    if (r == 0) return Wait::kTimeout;
    if (r < 0) {
        if (errno == EINTR) return Wait::kTimeout;
        Drop(std::strerror(errno));
        return Wait::kLost;
    }
    if (!PQconsumeInput(conn_)) {
        std::fprintf(stderr, "renderer: DB read failed: %s", PQerrorMessage(conn_));
        Drop("read failed");
        return Wait::kLost;
    }
    return DrainNotifies() ? Wait::kNotified : Wait::kTimeout;
}
