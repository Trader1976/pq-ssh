# pq-ssh Architecture

pq-ssh is a Qt (Widgets) desktop SSH client. It aims to provide a clean GUI for managing SSH profiles, connecting to hosts, and working in an embedded terminal view. It also includes a workflow for installing an SSH public key on a remote server in an idempotent way.

This document describes the current architecture at a module level and the main execution flows.

---

## Goals

- Simple, profile-based SSH connections (host/user/port/auth).
- Embedded interactive shell/terminal inside the app.
- Clear separation between UI, SSH/session logic, and terminal presentation.
- Repeatable (“idempotent”) remote public-key installation.
- Predictable theming across platforms/builds.

---

## Non-goals

- Replacing OpenSSH server-side components.
- Implementing advanced SSH features unless explicitly planned (agent forwarding, port forwarding, jump hosts, etc.).
- Hard-coupling the app to any specific identity system (out of scope for now).

---

## High-level Modules

### UI Layer (Qt Widgets)

Responsible for user interaction and orchestration.

Typical responsibilities:
- Profile list and profile selection.
- Connecting/disconnecting.
- Showing connection status, logs, and errors.
- Triggering actions like “Install public key on server”.

Key characteristics:
- Owns the main window widgets and wires signals/slots.
- Delegates all network/session work to the SSH layer.
- Receives events/callbacks and updates UI accordingly.

---

### Profiles & Configuration

Stores and loads connection profiles.

Typical fields:
- Display name
- Host, port, username
- Authentication method (password, key path, etc.)
- Optional per-profile UI preferences (future)

Design notes:
- Profiles are stored in a user-accessible config location (JSON-based).
- UI should not require the user to edit JSON directly (profiles editor exists/expected).

---

### SSH Session Layer (libssh wrapper)

Implements SSH connection handling and exposes a higher-level API to the UI.

Typical responsibilities:
- Create SSH session, set options (host/user/port).
- Authenticate (password and/or key-based).
- Open a PTY/channel for an interactive shell.
- Read/write loop for shell I/O.
- Clean shutdown and error propagation.

Design notes:
- Runs potentially blocking operations off the UI thread.
- Emits structured events (connected, auth failed, disconnected, output available, error text).
- Does not own UI objects.

---

### Terminal Presentation (qtermwidget integration)

Renders the shell as a terminal-like widget inside the app.

Typical responsibilities:
- Display remote output and accept user input.
- Provide consistent terminal colors/theme.
- Provide copy/paste, selection, scrollback.
- Optional drag/drop behaviors (download/upload features may live around here, but should be kept modular).

Design notes:
- Terminal widget should stay responsive even when SSH output is heavy.
- Terminal theme should be applied early so the initial screen matches expected colors.

---

### Key Installation Workflow (Remote authorized_keys management)

Provides a guided UI flow to install a public key on the remote server.

User flow:
1. User triggers “Install public key” action.
2. App confirms host/user/port and shows key preview.
3. App connects (typically password auth) and runs remote commands:
   - Ensure `~/.ssh` exists and has correct permissions:
     - `mkdir -p ~/.ssh && chmod 700 ~/.ssh`
   - Backup existing `authorized_keys` if present:
     - copy to `authorized_keys.bak_YYYYMMDD_HHMMSS`
   - Append the key **only if missing** (avoid duplicates).
   - Fix permissions:
     - `chmod 600 ~/.ssh/authorized_keys`
4. App reports success/failure with log details.

Design notes:
- Must be idempotent: repeating the action should not add duplicates.
- Must preserve existing keys (backup + append-only).
- Should avoid changing unrelated SSH server configuration.

Security notes:
- Always show the key to the user before installing.
- Avoid logging full private material (public key is OK; passwords are never logged).

---

## Runtime Data Flow

### Connect flow (typical)

1. User selects a profile.
2. UI invokes SSH layer to connect using profile settings.
3. SSH layer establishes a libssh session and authenticates.
4. SSH layer opens a shell channel + PTY.
5. Terminal widget is activated and connected to the I/O stream.
6. UI updates status to connected; logs show handshake/auth milestones.

---

## Threading Model

- UI runs on the Qt main thread.
- SSH connection/auth and channel I/O run off the UI thread.
- Communication uses signals/slots (queued connections) or a thread-safe event dispatch model.

Key rule:
- No direct UI manipulation from worker threads.

---

## Logging & Diagnostics

- UI should surface a readable log (session lifecycle, errors).
- Internals may log additional details for debugging, but should not leak secrets.
- Prefer structured messages (component prefix + message) to make support easier.

Example prefixes:
- `[UI]`, `[SSH]`, `[TERM]`, `[KEYS]`

---

## Theming

- App provides a consistent dark theme for core widgets.
- Terminal theme is handled separately (qtermwidget color scheme).
- Theme assets must ship with the application so the user sees the same result on every install.

---

## Future Extensions (non-binding)

Potential areas to expand later:
- Better profile editor UX (validation, import/export).
- Safer key management UI (detect duplicates, show fingerprints, expiry metadata).
- Optional SFTP features for download/upload in a dedicated module.
- Connection features: jump hosts, keepalive tuning, known_hosts UI, etc.

(These are intentionally non-committal and can be revised.)

---

## Notes for Contributors

- Keep UI logic (widgets, dialogs) separate from SSH/libssh calls.
- Avoid blocking the UI thread.
- Prefer small, testable units for SSH and key-install logic.
- Document any “why” decisions in code comments near the relevant logic.
