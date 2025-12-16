# pq-ssh Architecture

pq-ssh is a Qt (Widgets) desktop SSH client built on top of libssh and qtermwidget.  
It provides a profile-based GUI for connecting to SSH servers, working in an embedded terminal, and managing SSH keys, including post-quantum keys.

This document describes the architecture at a module and responsibility level, focusing on current, implemented behavior.

---

## Goals

- Simple, profile-based SSH connections (host / user / port).
- Embedded interactive terminal inside the application.
- Clear separation between UI, SSH/session logic, and terminal presentation.
- Safe, repeatable (“idempotent”) remote public-key installation.
- Consistent theming across platforms and builds.
- Optional verbose diagnostics gated behind an explicit debug mode.

---

## Non-goals

- Replacing OpenSSH server-side components.
- Requiring server-side configuration changes.
- Implementing identity or naming systems not yet defined.
- Acting as a full SSH feature superset (only planned features are implemented).

---

## Repository Layout

/
├── src/ # Application source code (Qt / C++)
│ ├── main.cpp # Application entry point
│ ├── MainWindow.* # Main UI orchestration
│ ├── AppTheme.* # Global Qt widget theming
│ ├── Logger.* # Application logging utilities
│
│ ├── ProfileStore.* # Profile persistence (JSON-backed)
│ ├── SshProfile.h # SSH profile data model
│ ├── ProfilesEditorDialog.* # Profile editor UI
│
│ ├── SshClient.* # libssh session wrapper
│ ├── SshShellWorker.* # SSH shell execution worker
│ ├── SshShellHelpers.h # Shared helpers for shell/PTY setup
│
│ ├── ShellManager.* # Lifecycle management of shell sessions
│ ├── TerminalView.* # Terminal container widget
│ ├── CpunkTermWidget.* # qtermwidget integration and fixes
│
│ ├── KeyGeneratorDialog.* # Key generation UI
│ ├── DilithiumKeyCrypto.* # Post-quantum key cryptographic operations
│ ├── KeyMetadataUtils.* # Key metadata parsing and expiration handling
│
│ ├── ThemeInstaller.* # Terminal color theme installation
│ └── SSH_KeyTypeSpecification.html # Reference documentation
│
├── resources/ # Qt resources bundled into the binary
│ ├── color-schemes/ # Terminal color themes
│ ├── docs/ # User manual and help documents
│ └── pqssh_resources.qrc # Qt resource manifest
│
├── profiles/
│ └── profiles.json # User SSH profiles (runtime data)
│
├── ARCHITECTURE.md # Architectural overview
├── README.md # Project overview
├── CMakeLists.txt # Build configuration
├── LICENSE
├── DUAL_LICENSE.md
└── THIRD_PARTY_LICENSES.md


---

## High-level Modules

### Main UI Layer

**Files:**
- `MainWindow.h/.cpp`
- `AppTheme.h/.cpp`
- `Logger.h/.cpp`

Responsibilities:
- Owns the main window, menus, dialogs, and layout.
- Orchestrates user actions (connect, disconnect, key install, profile edit).
- Applies global Qt widget theming.
- Displays logs, status messages, and user-facing errors.
- Controls whether verbose SSH diagnostics are shown (debug-gated).

Design notes:
- UI code does not directly perform blocking SSH operations.
- All long-running or network work is delegated to worker components.

---

### Profiles & Configuration

**Files:**
- `ProfileStore.h/.cpp`
- `SshProfile.h`
- `ProfilesEditorDialog.h/.cpp`
- `profiles/profiles.json`

Responsibilities:
- Load and save SSH profiles from JSON.
- Provide a profile editor UI so users do not need to edit JSON manually.
- Act as the single source of truth for connection parameters.

Design notes:
- Profiles are user runtime data, not bundled resources.
- Profile changes should immediately reflect in the UI.

---

### SSH Session & Shell Execution

**Files:**
- `SshClient.h/.cpp`
- `SshShellWorker.h/.cpp`
- `SshShellHelpers.h`
- `ShellManager.h/.cpp`

Responsibilities:
- Wrap libssh session creation and configuration.
- Perform authentication (password and/or key-based).
- Open PTY-backed shell channels.
- Manage lifecycle of SSH sessions and shell workers.
- Ensure sessions are closed cleanly when terminals are closed.

Design notes:
- SSH operations run off the UI thread.
- Worker components communicate with the UI via signals/slots.
- Resource cleanup is explicit and defensive.

---

### Terminal Integration

**Files:**
- `TerminalView.h/.cpp`
- `CpunkTermWidget.h/.cpp`

Responsibilities:
- Embed an interactive terminal widget inside the Qt UI.
- Bridge SSH shell I/O to qtermwidget.
- Apply terminal color schemes and visual fixes.
- Prevent accidental fallback to a local shell.

Design notes:
- Terminal must remain responsive even under heavy output.
- Terminal theming is applied early to avoid visual flicker.

---

### Key Management & Cryptography

**Files:**
- `KeyGeneratorDialog.h/.cpp`
- `DilithiumKeyCrypto.h/.cpp`
- `KeyMetadataUtils.h/.cpp`

Responsibilities:
- Generate SSH-compatible keys, including post-quantum keys.
- Handle cryptographic operations in a dedicated module.
- Parse and evaluate key metadata (expiration, validity).
- Expose safe, user-facing workflows for key installation.

Design notes:
- Cryptographic logic is isolated from UI logic.
- Debug/test functionality is hidden unless explicitly enabled.
- Sensitive material is handled carefully and never logged.

---

### Key Installation Workflow (Remote authorized_keys Management)

User flow:
1. User initiates “Install public key”.
2. Application confirms host, user, port, and shows a key preview.
3. Application connects to the server (typically using password auth).
4. Remote commands are executed:
   - Ensure `~/.ssh` exists with correct permissions:
     - `mkdir -p ~/.ssh && chmod 700 ~/.ssh`
   - If `authorized_keys` exists, create a timestamped backup.
   - Append the public key **only if it is not already present**.
   - Fix permissions:
     - `chmod 600 ~/.ssh/authorized_keys`
5. UI reports success or failure with clear diagnostics.

Design notes:
- Workflow is idempotent: repeating it does not duplicate keys.
- Existing keys are preserved.
- No SSH server configuration is modified.

---

### Themes & Resources

**Files / directories:**
- `ThemeInstaller.h/.cpp`
- `resources/color-schemes/`
- `resources/docs/`
- `resources/pqssh_resources.qrc`

Responsibilities:
- Ship terminal color schemes with the application.
- Ensure consistent terminal appearance across installs.
- Bundle user manual and help documents into the binary.

---

## Runtime Data Flow

### Typical Connect Flow

1. User selects a profile.
2. UI invokes the SSH layer with profile parameters.
3. SSH session is created and authenticated.
4. A PTY-backed shell channel is opened.
5. Terminal widget is activated and connected to shell I/O.
6. UI updates connection state and logs progress.

---

## Threading Model

- Qt UI runs on the main thread.
- SSH sessions and shell workers run on background threads.
- Communication uses Qt signals/slots with queued connections.

Rule:
- Worker threads never manipulate UI widgets directly.

---

## Logging & Diagnostics

- Application provides a user-accessible log file.
- Logs include connection lifecycle and errors.
- Verbose SSH diagnostics are hidden unless debug mode is enabled.
- Secrets (passwords, private keys) are never logged.

Example prefixes:
- `[UI]`, `[SSH]`, `[SHELL]`, `[TERM]`, `[KEYS]`

---

## Theming

- Global Qt widget theme is applied via `AppTheme`.
- Terminal colors are handled independently via shipped color schemes.
- All theme assets are bundled to ensure consistent appearance.

---

## Notes for Contributors

- Keep UI logic separate from SSH and cryptographic logic.
- Do not block the UI thread.
- Prefer explicit ownership and cleanup of SSH resources.
- Document *why* decisions are made, not just *what* the code does.
