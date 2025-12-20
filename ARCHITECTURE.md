pq-ssh Architecture

pq-ssh is a Qt (Widgets)–based desktop SSH client built on top of libssh and qtermwidget.
It focuses on a clean, profile-driven UI, embedded terminals, safe SSH key handling, and experimental post-quantum (PQ) key management — without requiring any server-side changes.

This document describes the current implemented architecture of pq-ssh.
Speculative features and future directions are explicitly called out and not mixed with current behavior.

Goals

Simple, profile-based SSH connections (host, user, port).
Embedded interactive terminal inside the application.
Clear separation between UI, SSH/session logic, and terminal rendering.
Safe, repeatable (idempotent) public-key installation on remote hosts.
Post-quantum key generation and at-rest protection (client-side only).
Deterministic identity derivation (mnemonic-based) for future workflows.
Consistent theming across platforms and installations.
Explicit control over debug verbosity (PQ and SSH diagnostics).

Non-Goals (Current Scope)

Replacing OpenSSH server-side components.
Requiring server-side configuration changes.
Using post-quantum keys for SSH authentication on stock OpenSSH servers.
Implementing decentralized identity (DNA identity, agents, etc.).
Acting as a full OpenSSH feature superset.
Acting as an SSH agent or key agent replacement.

/
├── src/
│ ├── main.cpp                     # Application entry point
│ ├── MainWindow.*                 # Main UI orchestration
│ ├── AppTheme.*                   # Global Qt widget theming
│ ├── Logger.*                     # Centralized logging
│
│ ├── ProfileStore.*               # Profile persistence (JSON-backed)
│ ├── SshProfile.h                 # SSH profile data model
│ ├── ProfilesEditorDialog.*       # Profile editor UI (incl. macros & groups)
│
│ ├── SshClient.*                  # libssh session wrapper
│ ├── SshShellWorker.*             # SSH PTY shell worker
│ ├── SshShellHelpers.h            # Shell / PTY helpers
│
│ ├── TerminalView.*               # Lightweight terminal widget
│ ├── CpunkTermWidget.*            # qtermwidget integration & fixes
│
│ ├── KeyGeneratorDialog.*         # Key generation & inventory UI
│ ├── DilithiumKeyCrypto.*         # PQ private-key encryption (libsodium)
│ ├── IdentityDerivation.*         # Mnemonic → deterministic key derivation
│ ├── OpenSshEd25519Key.*          # OpenSSH Ed25519 key serialization
│ ├── KeyMetadataUtils.*           # Metadata parsing & expiration handling
│
│ ├── SshConfigParser.*            # ~/.ssh/config parser (read-only)
│ ├── SshConfigImportDialog.*      # OpenSSH config preview dialog
│ ├── SshConfigImportPlan.*        # Import decision engine
│ ├── SshConfigImportPlanDialog.*  # Import plan UI (create/update/skip)
│
│ ├── ThemeInstaller.*             # Terminal color scheme installer
│ └── SSH_KeyTypeSpecification.html# Experimental PQ SSH key draft (reference)
│
├── resources/
│ ├── color-schemes/               # Bundled terminal themes
│ ├── docs/                        # User manual
│ └── pqssh_resources.qrc          # Qt resource manifest
│
├── profiles/
│ └── profiles.json                # Runtime SSH profiles
│
├── ARCHITECTURE.md
├── README.md
├── CMakeLists.txt
├── LICENSE
├── DUAL_LICENSE.md
└── THIRD_PARTY_LICENSES.md

High-Level Modules
1. Main UI Layer

Files
MainWindow.h/.cpp
AppTheme.h/.cpp
Logger.h/.cpp

Responsibilities
Owns the main window, menus, dialogs, and layout.
Coordinates user actions such as connect, disconnect, key install, and profile editing.
Applies global Qt widget theming.
Displays logs, status messages, and errors.
Controls debug verbosity.

Design Notes
UI code never performs blocking SSH operations.
All network and shell work is delegated to worker components.
Errors are surfaced in a user-readable form.

2. Profiles & Configuration

Files
ProfileStore.h/.cpp
SshProfile.h
ProfilesEditorDialog.h/.cpp
profiles/profiles.json

Responsibilities
Load and save SSH profiles as JSON.
Provide a GUI editor so users never edit JSON manually.
Store connection details, terminal preferences, key authentication settings, profile groups, and hotkey macros.
Validate profiles before persistence.

Design Notes
Profiles are runtime user data, not bundled resources.
Editing is done on a working copy and committed only on Save.
Macro support is backward-compatible with legacy single-macro fields.

3. SSH Session & Shell Execution

Files
SshClient.h/.cpp
SshShellWorker.h/.cpp
SshShellHelpers.h

Responsibilities
Wrap libssh session creation and configuration.
Perform password and key-based authentication.
Execute remote commands and file transfers.
Open PTY-backed interactive shells.
Cleanly manage SSH session lifecycle.

Design Notes
SSH operations run outside the UI thread.
Worker communication uses Qt signals and slots.
Secrets are never logged.

4. Terminal Integration

Files
TerminalView.h/.cpp
CpunkTermWidget.h/.cpp

Responsibilities
Embed interactive terminals inside the UI.
Bridge SSH shell I/O to terminal widgets.
Apply terminal color schemes and font settings.
Support drag-and-drop file workflows.

Design Notes
TerminalView is lightweight and SSH-focused.
CpunkTermWidget is used where full terminal emulation is required.
Terminal appearance is isolated from global Qt styles.

5. Key Management & Cryptography

Files
KeyGeneratorDialog.h/.cpp
DilithiumKeyCrypto.h/.cpp
IdentityDerivation.h/.cpp
OpenSshEd25519Key.h/.cpp
KeyMetadataUtils.h/.cpp

Responsibilities
Generate SSH keys (Ed25519, RSA, experimental Dilithium5).
Deterministically derive Ed25519 keys from mnemonic phrases.
Encrypt Dilithium private keys at rest using Argon2id and XChaCha20-Poly1305.
Serialize OpenSSH-compatible public and private key formats.
Track key metadata such as expiration and status.

Important Current Behavior
Dilithium keys are not used for SSH authentication.
Deterministic identities are client-side only.
OpenSSH-compatible keys are always used when interacting with servers.

6. OpenSSH Config Import

Files
SshConfigParser.*
SshConfigImportDialog.*
SshConfigImportPlan.*
SshConfigImportPlanDialog.*

Responsibilities
Parse ~/.ssh/config safely and read-only.
Preview Host blocks, GLOBAL defaults, and warnings.
Build an explicit import plan describing create, update, skip, or invalid actions.
Require user confirmation before applying changes.

Design Notes
Parsing never modifies OpenSSH config files.
Import is opt-in and explicit.
Wildcards, invalid ports, and conflicts are handled defensively.

7. Remote Key Installation Workflow

User Flow
User selects a public key.
User selects a target profile.
Application confirms host, user, port, and key preview.
Application connects to the server.
Remote commands ensure ~/.ssh exists and has correct permissions.
authorized_keys is backed up if present.
Public key is appended only if missing.
Permissions are fixed.
UI reports success or failure.

Design Notes
Workflow is idempotent and repeatable.
No server-side configuration is modified.
Existing keys are preserved.

8. Themes & Resources

Files
ThemeInstaller.h/.cpp
resources/color-schemes/
resources/docs/
pqssh_resources.qrc

Responsibilities
Bundle terminal color schemes.
Ensure consistent appearance across installations.
Embed user documentation into the binary.

Threading Model

Qt UI runs on the main thread.
SSH sessions and shell workers run in background threads.
Qt signals and slots with queued connections are used.

Rule
Worker threads must never manipulate UI widgets directly.

Logging & Diagnostics

Centralized logging via Logger.
Log rotation supported.
Debug verbosity is explicit and opt-in.
Secrets are never logged.

Typical prefixes
[UI] [SSH] [SHELL] [TERM] [KEYS]

Notes for Contributors

Keep UI, SSH, terminal, and crypto logic strictly separated.
Do not block the UI thread.
Be explicit about ownership and cleanup.
Prefer clarity over cleverness.
Document why a design choice exists.

Status Summary

Implemented
Profile-based SSH connections.
Embedded terminals.
Profile grouping and macro support.
OpenSSH config preview and import planning.
Deterministic identity derivation (client-side).
Key generation and inventory.
Encrypted Dilithium private keys at rest.
Safe public-key installation.
Theming and resource bundling.

Explicitly Not Implemented
PQ SSH authentication.
SSH agent or agent forwarding.
DNA identity integration.
Server-side PQ support.