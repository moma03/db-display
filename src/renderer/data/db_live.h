#ifndef DB_LIVE_H
#define DB_LIVE_H

// db_live.h
// One persistent PostgreSQL session for the renderer's fetch thread.
//
//  * LISTENs on "timetable_update": the timetable-fetcher sends NOTIFY with
//    the EVA number after every fetch that changed stored data, so the
//    renderer re-queries only when there is something new.
//  * Keeps this display's row in "display_subscriptions" alive (heartbeat).
//    The fetcher polls subscribed stations faster, and fetches a station
//    outside its own list temporarily while some display shows it.
//
// Both channels and the table are defined in
// timetable-fetcher/src/db_manager.py -- keep the names in sync.
//
// Not thread-safe: owned and used by the fetch thread only.

#include <chrono>
#include <string>

#include "db_fetcher.h"

struct pg_conn;
typedef struct pg_conn PGconn;

class DbLiveSession {
public:
    enum class Wait { kNotified, kTimeout, kLost };

    // `display_id` names this panel in display_subscriptions; one row per id.
    DbLiveSession(const DBConnectionConfig &cfg, std::string display_id);
    ~DbLiveSession();   // unsubscribes (best effort) and closes

    DbLiveSession(const DbLiveSession &) = delete;
    DbLiveSession &operator=(const DbLiveSession &) = delete;

    // Connects, LISTENs and subscribes if not connected. Reconnect attempts
    // back off from 2 s to 60 s, so this is cheap to call every iteration.
    // Sets *fresh when a new connection was just made: anything announced
    // while disconnected was missed, so the caller should re-query.
    bool EnsureConnected(bool *fresh);

    // Waits up to timeout_ms for a notification about our station. Also
    // reports notifications that arrived while a query was running.
    Wait WaitForUpdate(int timeout_ms);

    // Refreshes this display's subscription row. Returns false only if the
    // connection is gone (a missing table is logged once and tolerated, so a
    // renderer still works against an older fetcher, just by polling).
    bool Heartbeat();

    // Removes the subscription row and tells the fetcher. Called on shutdown.
    void Unsubscribe();

    PGconn *conn() const { return conn_; }

private:
    bool Connect();
    void Drop(const char *why);
    bool Subscribe(bool announce);
    bool DrainNotifies();   // true if one of them is for our station

    DBConnectionConfig cfg_;
    std::string display_id_;
    std::string eva_str_;
    PGconn *conn_ = nullptr;
    bool subscriptions_ok_ = true;   // false once the table turned out missing

    std::chrono::steady_clock::time_point next_attempt_{};
    int backoff_s_ = 0;
};

// Default display id: "<hostname>:<shm path>", i.e. one row per panel.
std::string DefaultDisplayId(const std::string &shm_path);

#endif  // DB_LIVE_H
