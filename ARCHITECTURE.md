# pq-ssh Architecture

pq-ssh is a Qt (Widgets)–based desktop SSH client built on top of **libssh** and **qtermwidget**.  
It focuses on a clean, profile-driven UI, embedded terminals, safe SSH key handling, and **experimental post-quantum (PQ) key management** — without requiring any server-side changes.

This document describes the **current implemented architecture** of pq-ssh.  
Speculative features and future directions are explicitly called out and **not** mixed with current behavior.

---

## Goals

- Simple, profile-based SSH connections (host / user / port).
- Embedded interactive terminal inside the application.
- Clear separation between UI, SSH/session logic, and terminal rendering.
- Safe, repeatable (“idempotent”) public-key installation on remote hosts.
- Post-quantum **key generation and at-rest protection** (client-side only).
- Consistent theming across platforms and installations.
- Explicit control over debug verbosity (PQ / SSH diagnostics).

---

## Non-Goals (Current Scope)

- Replacing OpenSSH server-side components.
- Requiring server-side configuration changes.
- Using post-quantum keys for SSH authentication on stock OpenSSH servers.
- Implementing decentralized identity (DNA identity, agents, etc.).
- Acting as a full OpenSSH feature superset.

---

## Repository Layout

/
├── src/
│ ├── main.cpp # Application entry point
│ ├── MainWindow.* # Main UI orchestration
│ ├── AppTheme.* # Global Qt widget theming
│ ├── Logger.* # Centralized logging
│
│ ├── ProfileStore.* # Profile persistence (JSON-backed)
│ ├── SshProfile.h # SSH profile data model
│ ├── ProfilesEditorDialog.* # Profile editor UI
│
│ ├── SshClient.* # libssh session wrapper
│ ├── SshShellWorker.* # SSH PTY shell worker
│ ├── SshShellHelpers.h # Shell / PTY helpers
│
│ ├── TerminalView.* # Lightweight terminal widget (QPlainTextEdit)
│ ├── CpunkTermWidget.* # qtermwidget integration & fixes
│
│ ├── KeyGeneratorDialog.* # Key generation & inventory UI
│ ├── DilithiumKeyCrypto.* # PQ private-key encryption (libsodium)
│ ├── KeyMetadataUtils.* # Metadata parsing & expiration handling
│
│ ├── ThemeInstaller.* # Terminal color scheme installer
│ └── SSH_KeyTypeSpecification.html # Experimental PQ SSH key draft (reference)
│
├── resources/
│ ├── color-schemes/ # Bundled terminal themes
│ ├── docs/ # User manual
│ └── pqssh_resources.qrc # Qt resource manifest
│
├── profiles/
│ └── profiles.json # Runtime SSH profiles
│
├── ARCHITECTURE.md
├── README.md
├── CMakeLists.txt
├── LICENSE
├── DUAL_LICENSE.md
└── THIRD_PARTY_LICENSES.md

---

## High-Level Modules

### 1. Main UI Layer

**Files**
- `MainWindow.h/.cpp`
- `AppTheme.h/.cpp`
- `Logger.h/.cpp`

**Responsibilities**
- Owns the main window, menus, dialogs, and layout.
- Coordinates user actions (connect, disconnect, key install, profile edit).
- Applies global Qt widget theming.
- Displays logs, status messages, and errors.
- Controls debug verbosity (e.g. PQ / SSH diagnostics).

**Design Notes**
- UI code never performs blocking SSH operations.
- All network and shell work is delegated to worker components.
- Errors are surfaced in a user-readable form.

---

### 2. Profiles & Configuration

**Files**
- `ProfileStore.h/.cpp`
- `SshProfile.h`
- `ProfilesEditorDialog.h/.cpp`
- `profiles/profiles.json`

**Responsibilities**
- Load and save SSH profiles as JSON.
- Provide a GUI editor so users never edit JSON manually.
- Store terminal preferences (size, font, colors, history).
- Store optional key-based authentication settings.

**Design Notes**
- Profiles are runtime user data, not bundled resources.
- Profiles are validated before being saved.
- Defaults are provided when no configuration exists.

---

### 3. SSH Session & Shell Execution

**Files**
- `SshClient.h/.cpp`
- `SshShellWorker.h/.cpp`
- `SshShellHelpers.h`

**Responsibilities**
- Wrap libssh session creation and configuration.
- Perform password and/or key-based authentication.
- Execute remote commands and file transfers.
- Open PTY-backed interactive shells.
- Cleanly manage SSH session lifecycle.

**Design Notes**
- SSH operations run outside the UI thread.
- Worker communication uses Qt signals/slots.
- Resources are explicitly closed and cleaned up.
- Secrets are never logged.

---

### 4. Terminal Integration

**Files**
- `TerminalView.h/.cpp`
- `CpunkTermWidget.h/.cpp`

**Responsibilities**
- Embed interactive terminals inside the UI.
- Bridge SSH shell I/O to the terminal widgets.
- Apply terminal color schemes and font settings.
- Support drag-and-drop:
  - Local → remote (upload)
  - Remote → local (via SCP helper)

**Design Notes**
- `TerminalView` is a lightweight custom terminal for SSH shell I/O.
- `CpunkTermWidget` is used where a full terminal emulator is required.
- Terminal appearance is isolated from global Qt styles.

---

### 5. Key Management & Cryptography

**Files**
- `KeyGeneratorDialog.h/.cpp`
- `DilithiumKeyCrypto.h/.cpp`
- `KeyMetadataUtils.h/.cpp`

**Responsibilities**
- Generate SSH keys (Ed25519, RSA, experimental Dilithium5).
- Maintain a local key inventory with metadata.
- Encrypt Dilithium private keys at rest using libsodium.
- Track expiration, rotation, and status (active / expired / revoked).
- Provide safe UI workflows for key installation.

**Important Current Behavior**
- **Dilithium keys are NOT used for SSH authentication.**
- Dilithium private keys are:
  - Encrypted using Argon2id + XChaCha20-Poly1305
  - Stored as `.enc` files
- Only **OpenSSH-compatible public keys** are installed on servers.

---

### 6. Remote Key Installation Workflow

**User Flow**
1. User selects a public key in the Key Generator.
2. User chooses a target profile.
3. Application confirms host, user, port, and key preview.
4. Application connects to the server (typically via password auth).
5. Remote commands are executed:
   - Ensure `~/.ssh` exists with correct permissions  
     ```
     mkdir -p ~/.ssh && chmod 700 ~/.ssh
     ```
   - If `authorized_keys` exists, create a timestamped backup.
   - Append the public key **only if it is not already present**.
   - Fix permissions:
     ```
     chmod 600 ~/.ssh/authorized_keys
     ```
6. UI reports success or failure.

**Design Notes**
- Workflow is idempotent (safe to repeat).
- Existing keys are preserved.
- No server-side configuration is modified.

---

### 7. Themes & Resources

**Files**
- `ThemeInstaller.h/.cpp`
- `resources/color-schemes/`
- `resources/docs/`
- `resources/pqssh_resources.qrc`

**Responsibilities**
- Bundle terminal color schemes with the application.
- Install schemes to system or user locations as appropriate.
- Ensure consistent terminal appearance across installs.
- Bundle user documentation into the binary.

---

## Runtime Data Flow

### Typical Connect Flow

1. User selects a profile.
2. UI requests connection via `SshClient`.
3. SSH session is created and authenticated.
4. A PTY-backed shell channel is opened.
5. Terminal widget is attached to shell I/O.
6. UI updates connection state and logs progress.

---

## Threading Model

- Qt UI runs on the main thread.
- SSH sessions and shell workers run in background threads.
- Communication uses Qt signals/slots with queued connections.

**Rule**
- Worker threads must never manipulate UI widgets directly.

---

## Logging & Diagnostics

- Centralized logging via `Logger`.
- Log file stored in application data directory.
- Log rotation is supported.
- Verbose SSH / PQ diagnostics are gated behind debug flags.
- Secrets (passwords, private keys) are never logged.

**Typical prefixes**
- `[UI]`, `[SSH]`, `[SHELL]`, `[TERM]`, `[KEYS]`

---

## Notes for Contributors

- Keep UI, SSH, terminal, and crypto logic strictly separated.
- Do not block the UI thread.
- Be explicit about ownership and cleanup.
- Prefer clear, defensive code over clever abstractions.
- Document **why** a design choice exists, not just **what** it does.

---

## Status Summary

**Implemented**
- Profile-based SSH connections
- Embedded terminals
- Key generation and inventory
- Encrypted Dilithium private keys (at rest)
- Safe public-key installation
- Theming and resource bundling

**Explicitly Not Implemented (Yet)**
- PQ SSH authentication
- SSH agent / agent forwarding
- DNA identity integration
- Server-side PQ support
