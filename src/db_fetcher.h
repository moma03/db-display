#ifndef DB_FETCHER_H
#define DB_FETCHER_H

// db_fetcher.h
// Fetches departure data from a PostgreSQL database populated by the
// Python main-node fetcher.  Uses libpq (the C PostgreSQL client library).

#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Data types that mirror the display logic in db_display.cc
// --------------------------------------------------------------------------

struct FetchedNote {
    int    id;
    std::string text;
};

struct FetchedDeparture {
    std::string platform;        // planned platform
    std::string cPlatform;       // changed platform (empty if unchanged)
    std::string line;            // e.g. "RE 11", "S 1", "ICE 123"
    std::string category;        // vehicle category, e.g. "RE", "S", "ICE", "Bus"
    std::string dest;            // final destination station name
    std::string pTime;           // planned time "HH:MM"
    std::string cTime;           // changed/actual time "HH:MM" (empty if on time)
    std::string stops;           // intermediate stops, comma-separated
    std::vector<FetchedNote> notes; // delay/cancellation reason text(s)
    bool cancelled = false;      // true when changed_status == 'c'
    bool destChanged = false;    // true when changed_destination differs from planned
};

struct FetchedStationInfo {
    std::string name;            // station display name
    int         eva = 0;         // EVA number
};

// --------------------------------------------------------------------------
// Database connection configuration
// --------------------------------------------------------------------------

struct DBConnectionConfig {
    std::string host     = "localhost";
    std::string port     = "5442";
    std::string dbname   = "stationsdb";
    std::string user     = "";
    std::string password = "";
    int         eva      = 8000297;   // default station EVA
};

// Load DB connection config from a JSON file.
// Reads the "database" object in the JSON.  Returns true on success.
bool LoadDBConnectionConfig(const std::string &json_path, DBConnectionConfig &cfg);

// --------------------------------------------------------------------------
// Fetcher interface
// --------------------------------------------------------------------------

// Fetch upcoming departures for the configured station.
// Returns the number of departures written into `out`.
// `ticker_out` is filled with any general ticker / disruption message (may be empty).
// On error returns -1 and logs to stderr.
int FetchDepartures(const DBConnectionConfig &cfg,
                    FetchedStationInfo &station_out,
                    std::vector<FetchedDeparture> &out,
                    std::string &ticker_out);

#endif // DB_FETCHER_H
