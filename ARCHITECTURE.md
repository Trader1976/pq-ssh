
•	pq-ssh Architecture
pq-ssh is a Qt (Widgets) desktop SSH client built on top of OpenSSH, libssh, and qtermwidget.
It provides a profile-based GUI for connecting to SSH servers, working in an interactive terminal, and managing SSH keys (including PQ-SSH’s Dilithium-related key tooling).
This document describes the architecture at a module and responsibility level, focusing on current, implemented behavior.
________________________________________
•	Goals
•	Simple, profile-based SSH connections (host / user / port).
•	Embedded interactive terminal sessions driven by OpenSSH.
•	Clear separation between UI orchestration, SSH/session logic, and terminal presentation.
•	Safe, repeatable (“idempotent”) remote public-key installation.
•	Consistent theming across platforms and builds.
•	Optional verbose diagnostics gated behind an explicit debug mode.
________________________________________
•	Non-goals
•	Replacing OpenSSH server-side components.
•	Requiring server-side configuration changes.
•	Implementing identity or naming systems not yet defined.
•	Acting as a full SSH feature superset (only implemented features are in scope).
________________________________________
•	Repository Layout
/
├── src/ # Application source code (Qt / C++)
│ ├── main.cpp # Application entry point (logger install, app init)
│ ├── MainWindow.* # Main UI orchestration + workflows
│ ├── AppTheme.* # Global Qt widget theming
│ ├── Logger.* # Application logging utilities
│
│ ├── ProfileStore.* # Profile persistence (JSON-backed)
│ ├── SshProfile.h # SSH profile data model
│ ├── ProfilesEditorDialog.* # Profile editor UI
│
│ ├── SshClient.* # libssh session wrapper (SFTP/control-plane + key install)
│ ├── SshShellWorker.* # SSH shell execution worker (legacy/auxiliary)
│ ├── SshShellHelpers.h # Shared helpers for shell/PTY setup
│
│ ├── ShellManager.* # Lifecycle management of shell sessions (auxiliary)
│ ├── TerminalView.* # Terminal container widget (auxiliary)
│ ├── CpunkTermWidget.* # qtermwidget integration, drag/drop, autostart fixes
│
│ ├── KeyGeneratorDialog.* # Key generation + key inventory UI
│ ├── DilithiumKeyCrypto.* # Dilithium key encryption/decryption helpers
│ ├── KeyMetadataUtils.* # Key metadata parsing + expiration handling
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
│ └── profiles.json # Default profiles shipped with the repo
│
├── ARCHITECTURE.md # Architectural overview (this document)
├── README.md # Project overview
├── CMakeLists.txt # Build configuration
├── LICENSE
├── DUAL_LICENSE.md
└── THIRD_PARTY_LICENSES.md
________________________________________
•	High-level Modules
•	Main UI Layer
Files:
•	MainWindow.h/.cpp
•	AppTheme.h/.cpp
•	Logger.h/.cpp
Responsibilities:
•	Owns the main window, menus, dialogs, and layout.
•	Orchestrates user actions (connect, disconnect, key install, profile edit).
•	Applies global Qt widget theming.
•	Displays logs, status messages, and user-facing errors.
•	Controls whether verbose diagnostics are shown in the UI (debug-gated).
Design notes:
•	UI code avoids blocking network work (libssh connect runs async).
•	OpenSSH terminal sessions are launched as child processes via the terminal widget.
•	Detailed logs go to file; UI is intentionally quieter unless PQ debug is enabled.
________________________________________
•	Profiles & Configuration
Files:
•	ProfileStore.h/.cpp
•	SshProfile.h
•	ProfilesEditorDialog.h/.cpp
•	profiles/profiles.json
Responsibilities:
•	Load and save SSH profiles from JSON (single source of truth for connection parameters).
•	Provide a profile editor UI so users do not need to edit JSON manually.
•	Apply selected profile values to the UI (host line, debug toggle, etc.).
Design notes:
•	Profiles are configuration, not runtime session state.
•	Profile changes should immediately reflect in the UI.
________________________________________
•	Connection Model (Two Planes)
pq-ssh uses two related but independent connection mechanisms:
1.	Interactive shell plane (OpenSSH)
o	Used for the actual user terminal session.
o	Launched as an ssh process inside qtermwidget (wrapped by CpunkTermWidget).
2.	Control plane (libssh via SshClient)
o	Used for SFTP and server-side operations (key install, uploads, remote pwd).
o	Connected asynchronously and may fail without breaking the terminal session.
This split is intentional:
•	The terminal experience stays close to standard OpenSSH behavior.
•	libssh provides structured APIs for file transfer and controlled remote file edits.
________________________________________
•	SSH Session & Control Plane (SshClient)
Files:
•	SshClient.h/.cpp
Responsibilities (current behavior):
•	Establish and hold a libssh session for a selected profile.
•	Provide SFTP helpers (upload bytes, read remote pwd).
•	Implement safe remote public key installation into ~/.ssh/authorized_keys.
•	Provide a passphrase provider callback (UI supplies passphrase on demand).
Design notes:
•	libssh connection is created in the background (via QtConcurrent / QFutureWatcher).
•	Failure degrades features (SFTP/key install) but does not stop OpenSSH terminal sessions.
•	No secrets are logged; errors are surfaced to the UI safely.
________________________________________
•	Terminal Integration (Interactive Shell)
Files:
•	CpunkTermWidget.h/.cpp
•	(auxiliary / evolving: TerminalView.*, ShellManager.*, SshShellWorker.*)
Responsibilities (current behavior in MainWindow):
•	Create an interactive terminal widget and run OpenSSH inside it.
•	Apply per-profile terminal options (history size, font size, color scheme).
•	Prevent accidental fallback to a local shell by using exec ssh.
•	Support drag-and-drop upload integration.
Design notes:
•	The terminal must accept input immediately (focus enforcement).
•	Terminal styling is protected from global Qt stylesheet bleed.
•	“WhiteOnBlack” is forced to true black where needed to avoid gray gutters/flicker.
________________________________________
•	PQ Awareness (Best-effort Probe)
Files:
•	Implemented primarily in MainWindow.cpp via QProcess running ssh.
Responsibilities:
•	Perform a short-lived OpenSSH probe using:
o	KexAlgorithms=sntrup761x25519-sha512@openssh.com
o	authentication disabled (PreferredAuthentications=none, BatchMode=yes, etc.)
•	Parse stderr to decide whether PQ KEX is available on the target.
•	Update a UI label:
o	PQ: ACTIVE (green) or PQ: OFF (red)
Design notes:
•	The probe is informational and does not control the actual terminal session.
•	Probe noise is printed to the UI only when PQ debug is enabled; always logged to file.
________________________________________
•	Key Management & Cryptography
Files:
•	KeyGeneratorDialog.h/.cpp
•	DilithiumKeyCrypto.h/.cpp
•	KeyMetadataUtils.h/.cpp
Responsibilities:
•	Key UI flows (generation/inventory via KeyGeneratorDialog).
•	Key metadata parsing and expiration handling (KeyMetadataUtils).
•	Encrypted Dilithium key operations and dev-only validation tools (DilithiumKeyCrypto).
Design notes:
•	Cryptographic logic is isolated from UI orchestration.
•	Debug/test functionality is hidden unless explicitly enabled (PQ debug).
•	Sensitive material is handled carefully:
o	plaintext is not logged
o	decrypted buffers are wiped (sodium_memzero) when used in dev tests
________________________________________
•	Key Installation Workflow (Remote authorized_keys Management)
This workflow appends an OpenSSH public key line to the remote ~/.ssh/authorized_keys safely and idempotently.
User flow:
1.	User initiates “Install public key” (from Key Generator or menu).
2.	Application confirms:
o	profile name
o	user/host/port
o	remote path ~/.ssh/authorized_keys
o	key type + short preview
3.	Application ensures a libssh session exists (connects if needed).
4.	Server-side operations are performed (via SshClient):
o	Ensure ~/.ssh exists and permissions are correct.
o	Backup existing authorized_keys if present.
o	Append the key only if missing.
o	Enforce permissions on authorized_keys.
Design notes:
•	Workflow is idempotent: repeating it does not duplicate keys.
•	Existing keys are preserved and backed up.
•	No SSH server configuration is modified.
________________________________________
•	Themes & Resources
Files / directories:
•	ThemeInstaller.h/.cpp
•	resources/color-schemes/
•	resources/docs/
•	resources/pqssh_resources.qrc
•	AppTheme.h/.cpp
Responsibilities:
•	Ship terminal color schemes with the application.
•	Ensure consistent terminal appearance across installs.
•	Bundle user manual and help documents into the binary (Qt resources).
•	Apply a consistent dark theme to normal Qt widgets.
Design notes:
•	Terminal colors are handled independently from global widget stylesheet.
•	Resource-based docs allow offline manual viewing.
________________________________________
•	Runtime Data Flow
•	Typical Connect Flow (What actually happens today)
1.	User selects a profile (UI fills user@host[:port], debug toggle, etc.).
2.	User clicks Connect.
3.	MainWindow:
o	logs a session id for the attempt
o	opens an interactive terminal session (OpenSSH in qtermwidget)
o	starts a PQ KEX probe (short-lived ssh process)
o	starts an async libssh connect (SFTP/control plane) when supported by profile key type
4.	UI updates:
o	Connect/Disconnect button states
o	Status text
o	PQ status label once probe completes
5.	When the terminal exits:
o	terminal tab/window closes
o	libssh is disconnected
o	UI returns to disconnected state
Key rule:
•	The interactive terminal is the “real” session.
libssh is a helper channel for SFTP + key installation.
________________________________________
•	Drag & Drop Upload Behavior
Drag-drop is handled from the terminal widget to MainWindow::onFileDropped().
Behavior:
•	If libssh is not connected:
o	save dropped file to ~/pqssh_drops/ (local fallback)
•	If libssh is connected:
o	upload bytes via SFTP to $PWD/<filename> on the remote host
Design notes:
•	This is a safe degradation path: users never lose a dropped file.
•	Remote destination is intentionally simple and predictable (current directory).
________________________________________
•	Threading Model
•	Qt UI runs on the main thread.
•	libssh connect and other potentially blocking operations run in background threads (QtConcurrent).
•	Communication uses Qt signals/slots or watcher callbacks.
Rule:
•	Worker threads never manipulate UI widgets directly.
Note:
•	The OpenSSH terminal session itself is a child process managed by the terminal widget.
________________________________________
•	Logging & Diagnostics
•	Application provides a user-accessible log file (Logger).
•	Logs include connection lifecycle, probe results, and libssh diagnostics.
•	UI log output is filtered:
o	normal mode hides noisy diagnostics
o	PQ debug mode shows more detail
•	Secrets (passwords, private keys) are never logged.
Common prefixes:
•	[UI], [CONNECT], [SFTP], [PQ-PROBE], [TERM], [KEYS], [DEV], [SECURITY]
Design notes:
•	pq-ssh uses a “dual channel” approach:
o	file log = always detailed
o	UI terminal = user-friendly unless debug enabled
________________________________________
•	User Manual
•	Manual is embedded as a Qt resource (qrc:/docs/user-manual.html).
•	Displayed in a QTextBrowser dialog with dark styling and external links enabled.
•	If resource is missing, UI shows a warning line.
________________________________________
•	Theming
•	Global Qt widget theme is applied via AppTheme.
•	Terminal colors are handled via shipped color schemes and explicit safeguards.
•	Terminal widgets are protected from global stylesheets to avoid unintended palette bleed.
________________________________________
•	Notes for Contributors
•	Keep UI orchestration separate from SSH and cryptographic logic.
•	Do not block the UI thread.
•	Prefer explicit ownership and cleanup of SSH resources.
•	Degrade features gracefully:
o	terminal should still work even if libssh fails
•	Document why decisions are made, not just what the code does.

