# pq-ssh Architecture

pq-ssh is a Qt Widgets–based desktop SSH client built around two separate SSH paths:

- OpenSSH (external `ssh` process) for the interactive terminal
- libssh for SFTP and small remote command helpers

It focuses on a profile-driven UI, embedded terminals, safe key workflows, deterministic client-side identity derivation, and practical post-quantum integration where it fits today, without requiring server-side changes.

This document describes the currently implemented architecture of pq-ssh. Future ideas and speculative features are called out separately and are not mixed with current behavior.

---

## Goals

- Simple, profile-based SSH connections (`host`, `user`, `port`)
- Embedded interactive terminal inside the application
- Clear separation between UI, SSH/session logic, terminal rendering, scheduling, and cryptography
- Safe, repeatable public-key installation on remote hosts
- Deterministic identity derivation from a mnemonic phrase
- Post-quantum-safe identity roots (ML-DSA-87 / Dilithium family)
- OpenSSH-compatible SSH authentication without server-side changes
- SFTP and remote file operations from inside the app
- OpenSSH config import with explicit user review
- Consistent theming across installations
- Explicit control over debug verbosity and audit/log output
- Portable Linux packaging via AppImage

---

## Non-Goals (Current Scope)

- Replacing OpenSSH server-side components
- Requiring server-side configuration changes
- Using post-quantum keys directly for SSH authentication
- Acting as an SSH agent or agent replacement
- Decentralized identity resolution (DNA networking, DHT, agents, etc.)
- Full OpenSSH feature parity
- Server-side PQ deployment automation
- PQ-authenticated SSH login end-to-end

---

## High-Level Architecture

pq-ssh intentionally splits SSH responsibilities into separate layers.

### 1. Interactive terminal path

The terminal UI uses:

- qtermwidget for terminal rendering
- external `ssh` (spawned as a child process) for the actual interactive shell session

This path gives pq-ssh strong OpenSSH compatibility while avoiding the complexity of implementing a full interactive shell protocol stack in-process.

### 2. Programmatic SSH path

Programmatic operations use libssh:

- SFTP
- remote file listing and stat
- upload and download
- small remote command execution
- authorized_keys installation workflow

This path is wrapped by `SshClient`.

### 3. Identity and crypto path

Mnemonic-based identity and key derivation is fully local:

- BIP39-style 24-word mnemonic handling
- deterministic derivation of a PQ identity root
- deterministic derivation of an Ed25519 SSH identity
- encrypted storage of PQ private material where applicable

---

## Source Tree Overview

This is a practical overview of the currently relevant structure.

    /
    ├── src/
    │   ├── main.cpp
    │   ├── MainWindow.*                   # Main UI orchestration
    │   ├── AppTheme.*                     # Global Qt widget styling
    │   ├── ThemeInstaller.*               # Terminal color scheme installation
    │   ├── Logger.*                       # Runtime logging
    │   ├── AuditLogger.*                  # Audit trail logging
    │   │
    │   ├── ProfileStore.*                 # Profile persistence
    │   ├── SshProfile.h                   # SSH profile data model
    │   ├── ProfilesEditorDialog.*         # Profile editor UI
    │   │
    │   ├── SshClient.*                    # libssh wrapper for SFTP + exec helpers
    │   ├── SshShellWorker.*               # Shell-related worker / legacy helper path
    │   ├── ShellManager.*                 # Shell/session management helpers
    │   ├── TerminalView.*                 # Terminal UI support
    │   ├── CpunkTermWidget.*              # qtermwidget integration and fixes
    │   │
    │   ├── FilesTab.*                     # Remote file browser / SFTP UI
    │   ├── RemoteDropTable.*              # Drag/drop related UI support
    │   │
    │   ├── KeyGeneratorDialog.*           # SSH key generation UI
    │   ├── OpenSshEd25519Key.*            # OpenSSH Ed25519 serialization
    │   ├── KeyMetadataUtils.*             # Key metadata / expiration helpers
    │   ├── DnaIdentityDerivation.*        # Mnemonic → PQ identity + deterministic SSH seed
    │   ├── IdentityManagerDialog.*        # Mnemonic identity UI
    │   │
    │   ├── SshConfigParser.*              # ~/.ssh/config parser
    │   ├── SshConfigImportDialog.*        # Import preview UI
    │   ├── SshConfigImportPlan.*          # Import plan engine
    │   ├── SshConfigImportPlanDialog.*    # Import plan UI
    │   │
    │   ├── SettingsDialog.*               # App settings UI
    │   ├── PortForwardingDialog.*         # Port forwarding editor
    │   │
    │   ├── ScheduledJob.h
    │   ├── ScheduledJobStore.*            # Persistent scheduled jobs
    │   ├── ScheduledJobsDialog.*          # Scheduled jobs UI
    │   │
    │   ├── Fleet/
    │   │   ├── FleetTypes.h
    │   │   ├── FleetExecutor.*
    │   │   └── FleetWindow.*
    │   │
    │   ├── Audit/
    │   │   ├── AuditLogViewerDialog.*
    │   │   ├── AuditLogModel.*
    │   │   └── AuditLogDelegate.*
    │   │
    │   └── dna_vendor/                    # Vendored PQ signing implementation
    │
    ├── resources/
    │   ├── color-schemes/
    │   ├── docs/
    │   ├── wordlists/
    │   └── pqssh_resources.qrc
    │
    ├── packaging/
    │   ├── pq-ssh.desktop
    │   └── icons/
    │
    ├── scripts/
    │   └── build-appimage-12.sh
    │
    ├── tools/                             # linuxdeploy / packaging helpers
    ├── ARCHITECTURE.md
    ├── README.md
    ├── CMakeLists.txt
    ├── LICENSE
    ├── DUAL_LICENSE.md
    └── THIRD_PARTY_LICENSES.md

---

## Runtime Data and Persistence

pq-ssh uses standard user-space paths instead of storing runtime state in the repository.

Typical paths:

- Profiles: `~/.config/CPUNK/pq-ssh/profiles.json`
- App settings / QSettings namespace: `CPUNK / pq-ssh`
- Key material: under user home, for example `~/.pq-ssh/...`
- Logs / audit data: application-local data paths managed by `Logger` and `AuditLogger`

Design intent:

- repository remains source-only
- runtime and user data lives under user config/data locations
- profile editing is done through the GUI, not manual repo file edits

---

## Module Breakdown

## 1. Main UI Layer

### Files

- `MainWindow.*`
- `AppTheme.*`
- `Logger.*`
- `AuditLogger.*`
- `SettingsDialog.*`

### Responsibilities

- Own the main window, menus, status bars, tabs, and dialogs
- Coordinate connect and disconnect flows
- Manage profile selection and profile-driven behavior
- Start interactive terminal sessions
- Coordinate SFTP readiness and file-tab state
- Display logs, warnings, status labels, and negotiated KEX indicators
- Apply global UI theme and user settings
- Drive high-level workflows such as:
    - key install
    - mnemonic identity recovery
    - config import
    - scheduled jobs
    - fleet operations

### Design Notes

- UI code should not block on network or crypto work
- Background work is delegated to helpers, `QProcess`, `QtConcurrent`, and service classes
- The UI distinguishes between:
    - terminal/OpenSSH KEX
    - SFTP/libssh KEX
- Debug verbosity is explicit and user-controlled

---

## 2. Profiles and Configuration

### Files

- `ProfileStore.*`
- `SshProfile.h`
- `ProfilesEditorDialog.*`

### Responsibilities

- Load and save SSH profiles as JSON
- Provide a GUI editor for profile data
- Store:
    - host
    - user
    - port
    - key settings
    - terminal preferences
    - macros
    - groups
    - port forwarding settings
    - debug options
- Preserve backward compatibility for older profile data where practical

### Design Notes

- Profiles are runtime user data, not source-controlled app config
- Editing is staged through the UI and committed on save
- Grouping and macro support are part of the live product, not experimental only
- Current profile persistence path is under the user config directory, not the repo-local `profiles/` folder

---

## 3. Interactive Terminal Path

### Primary Files

- `MainWindow.*`
- `CpunkTermWidget.*`
- `TerminalView.*`
- `ShellManager.*`

### Responsibilities

- Create terminal windows or tabs
- Launch external `ssh` via `bash -lc` / wrapped command flow
- Feed profile-derived SSH arguments into the launched process
- Apply terminal font, color scheme, and UI protection against global style bleed
- Support profile macros and terminal shortcuts
- Handle terminal window lifecycle and cleanup

### Current Behavior

The interactive terminal path is intentionally based on the host system's OpenSSH client.

This means:

- terminal compatibility follows system `ssh`
- interactive shell behavior is not limited by libssh terminal support
- host OpenSSH version affects interactive PQ KEX capabilities

### Current PQ Behavior

At present, the terminal/OpenSSH path explicitly prefers hybrid PQ using OpenSSH-compatible `sntrup761x25519-sha512@openssh.com`.

This path is separate from the libssh SFTP path and may negotiate differently.

---

## 4. libssh Session Layer

### Files

- `SshClient.*`

### Responsibilities

- Own and manage a libssh session
- Configure host, user, port, timeout, identity path, and KEX preference
- Authenticate using agent or public-key auto flow
- Emit negotiated KEX details back to the UI
- Provide helpers for:
    - SFTP init
    - remote stat/list
    - upload/download
    - remote `exec`
    - authorized_keys installation

### Design Notes

- `SshClient` is not a full interactive terminal engine
- It is used for programmatic SSH operations only
- Passphrase prompts are UI-injected through a callback
- Secrets are never logged

### Current KEX Behavior

With libssh 0.12, pq-ssh now prefers PQ-capable KEX for the programmatic libssh path in this order:

- `mlkem768x25519-sha256`
- `mlkem768nistp256-sha256`
- `sntrup761x25519-sha512@openssh.com`
- `sntrup761x25519-sha512`
- `curve25519-sha256`

The actual negotiated KEX depends on server support.

### Important Separation

- terminal/OpenSSH KEX is reported separately
- SFTP/libssh KEX is reported separately

This is intentional and reflects the real architecture.

---

## 5. SFTP and Remote File Operations

### Files

- `SshClient.*`
- `FilesTab.*`
- `RemoteDropTable.*`

### Responsibilities

- Remote directory listing
- Remote file stat
- Upload and download
- Transfer progress reporting
- Cancel handling
- Hash verification helpers
- UI integration for remote browsing

### Safety Behavior

Upload path uses safe temp-file patterns:

- write to temp name
- rename into place
- best-effort backup handling for replacement
- cleanup on cancel/error

Download path similarly uses temp-file write then final replace on local side.

### Integrity Helpers

Current code includes SHA-256 verification helpers for local vs remote file comparison.

---

## 6. Identity, Keys, and Cryptography

This is one of the most important architectural areas in pq-ssh.

### Files

- `DnaIdentityDerivation.*`
- `IdentityManagerDialog.*`
- `OpenSshEd25519Key.*`
- `KeyGeneratorDialog.*`
- `KeyMetadataUtils.*`
- `dna_vendor/`

### Identity Model (Current)

pq-ssh uses a single mnemonic-based identity root, from which:

- a post-quantum DNA identity is derived
- a classical Ed25519 SSH identity is deterministically derived

Both are generated locally and deterministically, with no server interaction.

### Derivation Pipeline

- 24-word mnemonic
- optional passphrase
- BIP39-compatible PBKDF2-HMAC-SHA512 (2048 rounds)
- 64-byte master seed

From that seed:

- PQ identity branch:
    - domain-separated derivation
    - deterministic ML-DSA-87 / Dilithium-derived keypair
    - SHA3-512 fingerprint
- SSH branch:
    - domain-separated derivation
    - deterministic Ed25519 seed
    - OpenSSH-compatible key serialization

### Important Current Behavior

- Dilithium / ML-DSA keys are not used for SSH authentication
- The PQ identity root is client-side and deterministic
- SSH authentication remains OpenSSH-compatible Ed25519
- No server-side changes are required
- Re-entering the same mnemonic recreates the same identity

---

## 7. OpenSSH Config Import

### Files

- `SshConfigParser.*`
- `SshConfigImportDialog.*`
- `SshConfigImportPlan.*`
- `SshConfigImportPlanDialog.*`

### Responsibilities

- Read and parse `~/.ssh/config`
- Preview `Host` entries
- Build an explicit import plan
- Require user confirmation before import

### Design Notes

- Import is user-reviewed
- Existing app profiles are not silently overwritten
- Config import is meant as a convenience bridge, not as a full OpenSSH config replacement engine

---

## 8. Remote Key Installation Workflow

### Primary File

- `SshClient.*`

### User Flow

1. User selects a generated or derived public key
2. User selects a target profile
3. pq-ssh validates host, user, and port
4. Application establishes a libssh connection if needed
5. Ensures `~/.ssh` exists with correct permissions
6. Reads existing `authorized_keys`
7. Creates backup when appropriate
8. Appends the key only if missing
9. Writes back atomically
10. Fixes permissions and reports result

### Design Notes

- Idempotent and repeatable
- Existing keys are preserved
- No server-side configuration changes required
- Failure paths try to avoid destructive partial updates

---

## 9. Themes and Terminal Styling

### Files

- `AppTheme.*`
- `ThemeInstaller.*`
- `resources/color-schemes/`
- `resources/pqssh_resources.qrc`

### Responsibilities

- Global Qt widget theming
- Terminal color scheme installation and use
- Font and palette normalization
- Protection against app-wide styles breaking terminal appearance

### Design Notes

pq-ssh treats the terminal as a special visual surface.

The code includes specific work to:

- force terminal color consistency
- prevent unwanted bold styling from global themes
- keep terminal surroundings visually aligned with terminal background

---

## 10. Scheduled Jobs and Fleet Operations

### Files

- `ScheduledJob.h`
- `ScheduledJobStore.*`
- `ScheduledJobsDialog.*`
- `Fleet/*`

### Responsibilities

- Save and load scheduled jobs
- Let the user define repeatable actions tied to profiles
- Run multi-host operations through Fleet UI
- Keep scheduling and bulk execution distinct from core terminal/SFTP code

### Design Notes

- scheduled jobs are persisted separately from normal profile editing
- fleet execution is a higher-level orchestration layer above profile connections
- these features expand pq-ssh beyond a single-session SSH launcher

---

## 11. Logging and Audit

### Files

- `Logger.*`
- `AuditLogger.*`
- `Audit/*`

### Responsibilities

- Centralized application log output
- Audit event recording
- Audit log viewer UI
- Status and diagnostics for user-visible workflows

### Typical Prefixes

- `[UI]`
- `[SSH]`
- `[TERM]`
- `[SFTP]`
- `[SFTP-KEX]`
- `[PQ-PROBE]`
- `[CONNECT]`
- `[SECURITY]`

### Design Notes

- secrets must never be logged
- audit is distinct from general runtime logging
- UI-visible logs are filtered by verbosity settings where appropriate

---

## 12. Threading and Process Model

### Current Model

- Qt UI runs on the main thread
- external terminal SSH runs in child processes (`QProcess`)
- libssh connect helpers may run through `QtConcurrent`
- queued signals and slots are used to return results to the UI

### Rule

Worker threads and child-process handlers must never manipulate UI widgets directly outside Qt-safe signal/slot patterns.

---

## 13. Packaging and Distribution

### Relevant Files

- `packaging/pq-ssh.desktop`
- `packaging/icons/...`
- `scripts/build-appimage-12.sh`
- `tools/linuxdeploy*`

### Current Distribution Model

Primary Linux distribution artifact is an AppImage.

The build process:

- builds pq-ssh in Release mode
- points CMake at private libssh 0.12
- copies runtime dependencies into the bundle
- packages with linuxdeploy
- verifies that bundled `libssh.so` is used from inside the AppImage payload

### Important Current Packaging Fact

The AppImage is expected to ship with bundled private libssh 0.12 for the libssh/SFTP path, while the interactive terminal path still uses the host system's OpenSSH client.

---

## Connection Flow Summary

## Interactive terminal flow

1. User selects a profile
2. `MainWindow` builds an `ssh` command line
3. pq-ssh launches external OpenSSH inside qtermwidget
4. terminal session runs until user exit or process end
5. terminal-specific KEX probing is shown separately

## SFTP / programmatic flow

1. User selects a profile
2. `MainWindow` triggers background connect through `SshClient`
3. `SshClient` configures libssh options
4. libssh negotiates KEX and authenticates
5. UI receives negotiated KEX via signal
6. `FilesTab` becomes ready for remote browsing and transfers

---

## Security Posture (Current)

### What pq-ssh does today

- keeps SSH authentication OpenSSH-compatible
- derives identities locally
- supports deterministic recovery from mnemonic
- separates terminal/OpenSSH from SFTP/libssh paths
- uses safe temp-file workflows for many file operations
- avoids logging secrets
- supports encrypted-at-rest handling for PQ-related private material where implemented

### What pq-ssh does not claim today

- PQ-authenticated SSH login
- post-quantum server identity verification end-to-end
- replacement of OpenSSH server-side mechanisms
- decentralized identity lookup or trust distribution

---

## Status Summary

## Implemented

- Profile-based SSH connections
- Embedded terminals via qtermwidget + external OpenSSH
- SFTP and remote file operations via libssh
- Profile grouping and macros
- OpenSSH config import planning
- Deterministic mnemonic-based identity
- Post-quantum DNA fingerprint derivation
- Deterministic Ed25519 SSH key derivation
- Safe public-key installation workflow
- Theming and resource bundling
- Scheduled jobs
- Fleet jobs UI
- Audit logging and audit viewer
- AppImage packaging with bundled private libssh 0.12
- Separate terminal/OpenSSH and SFTP/libssh KEX reporting

## Explicitly Not Implemented

- PQ SSH authentication
- SSH agent functionality
- DNA networking or decentralized resolution
- Server-side PQ deployment
- Full OpenSSH feature parity
- Complete replacement of host OpenSSH behavior

---

## Current Practical Summary

pq-ssh is best understood as:

- a Qt desktop SSH client
- with an embedded OpenSSH-driven interactive terminal
- a libssh-driven SFTP and helper-command layer
- deterministic mnemonic-based identity recovery
- a post-quantum identity root
- classical OpenSSH-compatible SSH authentication
- practical Linux packaging through AppImage

It is not a new SSH protocol stack or a server-side replacement. It is a pragmatic client application that combines familiar SSH compatibility with newer identity and PQ-oriented building blocks where they are currently practical.