#pragma once

#include <QString>
#include <QVector>

#include "SshProfile.h"   // Uses the shared SshProfile struct (do NOT redefine)

/*
    ProfileStore
    ------------
    Static utility class responsible for persisting SSH connection profiles.

    Responsibilities:
    - Define where profiles.json lives
    - Load profiles from disk (JSON)
    - Save profiles to disk (JSON)
    - Provide a default profile set for first-run / empty config

    Design notes:
    - This class is intentionally stateless.
    - All functions are static to keep usage simple and explicit.
    - JSON schema is forward-compatible (unknown fields are ignored on load).
*/

class ProfileStore
{
public:
    /*
        Returns the absolute path to profiles.json.

        Current behavior:
        - Profiles are stored inside the *project directory*:
              pq-ssh/profiles/profiles.json

        Rationale (for now):
        - Convenient during active development
        - Easy to inspect and version-control profiles if desired

        NOTE:
        - This may later move to QStandardPaths (AppConfigLocation)
          when pq-ssh becomes a packaged / installed application.
    */
    static QString configPath();

    /*
        Returns a vector of default profiles.

        Usage:
        - Called by the application when load() returns an empty list
        - Used to seed first-run experience

        Typical contents:
        - One "Localhost" profile
        - Reasonable terminal defaults
        - key_type = "auto"
    */
    static QVector<SshProfile> defaults();

    /*
        Save all profiles to disk.

        Parameters:
        - profiles: list of SshProfile objects to serialize
        - err: optional output string for human-readable error message

        Behavior:
        - Overwrites profiles.json atomically (truncate + write)
        - Creates profiles/ directory if missing
        - Returns true on success, false on failure

        NOTE:
        - Always writes key_type, even if "auto", to keep schema stable.
    */
    static bool save(const QVector<SshProfile>& profiles, QString* err = nullptr);

    /*
        Load profiles from disk.

        Parameters:
        - err: optional output string for error diagnostics

        Behavior:
        - If profiles.json does not exist:
              → returns empty vector (NOT an error)
        - If JSON is invalid:
              → returns empty vector and sets err
        - Profiles missing required fields (user/host) are skipped
        - Missing optional fields fall back to sensible defaults

        Typical caller behavior:
        - If load() returns empty:
              → call defaults() and optionally save()
    */
    static QVector<SshProfile> load(QString* err = nullptr);
};
