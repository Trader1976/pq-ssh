pq-ssh Architecture

pq-ssh is a Qt (Widgets)–based desktop SSH client built on top of libssh and qtermwidget.
It focuses on a clean, profile-driven UI, embedded terminals, safe SSH key handling, and client-side post-quantum identity derivation, without requiring any server-side changes.

This document describes the currently implemented architecture of pq-ssh.
Speculative features and future directions are explicitly called out and are not mixed with current behavior.

Goals

Simple, profile-based SSH connections (host, user, port)

Embedded interactive terminal inside the application

Clear separation between UI, SSH/session logic, terminal rendering, and cryptography

Safe, repeatable (idempotent) public-key installation on remote hosts

Deterministic identity derivation from a mnemonic phrase (client-side only)

Post-quantum–safe identity roots (Dilithium / ML-DSA-87)

OpenSSH compatibility without server-side changes

Consistent theming across platforms and installations

Explicit control over debug verbosity (PQ and SSH diagnostics)

Non-Goals (Current Scope)

Replacing OpenSSH server-side components

Requiring server-side configuration changes

Using post-quantum keys directly for SSH authentication

Acting as an SSH agent or key agent replacement

Decentralized identity resolution (DNA networking, agents, DHT, etc.)

Full OpenSSH feature parity

Source Tree Overview
/
├── src/
│ ├── main.cpp                     # Application entry point
│ ├── MainWindow.*                 # Main UI orchestration
│ ├── AppTheme.*                   # Global Qt widget theming
│ ├── Logger.*                     # Centralized logging
│
│ ├── ProfileStore.*               # Profile persistence (JSON-backed)
│ ├── SshProfile.h                 # SSH profile data model
│ ├── ProfilesEditorDialog.*       # Profile editor UI (macros & groups)
│
│ ├── SshClient.*                  # libssh session wrapper
│ ├── SshShellWorker.*             # SSH PTY shell worker
│ ├── SshShellHelpers.h            # Shell / PTY helpers
│
│ ├── TerminalView.*               # Lightweight terminal widget
│ ├── CpunkTermWidget.*            # qtermwidget integration & fixes
│
│ ├── KeyGeneratorDialog.*         # Classical SSH key generation UI
│ ├── DilithiumKeyCrypto.*         # PQ private-key encryption at rest
│ ├── IdentityDerivation.*         # BIP39-compatible seed derivation
│ ├── DnaIdentityDerivation.*      # DNA identity & fingerprint derivation
│ ├── IdentityManagerDialog.*      # Mnemonic-based identity UI
│ ├── OpenSshEd25519Key.*          # OpenSSH Ed25519 serialization
│ ├── KeyMetadataUtils.*           # Key metadata & expiration helpers
│
│ ├── SshConfigParser.*            # ~/.ssh/config parser (read-only)
│ ├── SshConfigImportDialog.*      # OpenSSH config preview dialog
│ ├── SshConfigImportPlan.*        # Import decision engine
│ ├── SshConfigImportPlanDialog.*  # Import plan UI
│
│ ├── ThemeInstaller.*             # Terminal color scheme installer
│ └── SSH_KeyTypeSpecification.html# Experimental PQ SSH draft (reference)
│
├── resources/
│ ├── color-schemes/
│ ├── docs/
│ └── pqssh_resources.qrc
│
├── profiles/
│ └── profiles.json
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

MainWindow.h / .cpp

AppTheme.h / .cpp

Logger.h / .cpp

Responsibilities
Owns the main window, menus, dialogs, and layout
Coordinates user actions (connect, disconnect, key install, identity derivation)
Applies global Qt widget theming
Displays logs, status messages, and errors
Controls debug verbosity

Design Notes
UI code never performs blocking operations
All SSH and cryptographic work is delegated to worker or service layers
Errors are always surfaced in a user-readable form

2. Profiles & Configuration

Files
ProfileStore.*
SshProfile.h
ProfilesEditorDialog.*
profiles/profiles.json
Responsibilities

Load and save SSH profiles as JSON
Provide a GUI editor (no manual JSON editing)
Store connection details, terminal preferences, authentication settings, and macros

Validate profiles before persistence

Design Notes
Profiles are user runtime data
Changes are staged and committed only on explicit save
Macro support is backward-compatible

3. SSH Session & Shell Execution

Files
SshClient.*
SshShellWorker.*
SshShellHelpers.h
Responsibilities

Wrap libssh session creation and configuration
Perform password and key-based authentication
Execute remote commands and file transfers
Open PTY-backed interactive shells
Manage SSH lifecycle cleanly

Design Notes
SSH work never runs on the UI thread
Qt signals and slots are used for communication
Secrets are never logged

4. Terminal Integration
Files
TerminalView.*
CpunkTermWidget.*
Responsibilities
Embed interactive terminals
Bridge SSH shell I/O to terminal widgets
Apply terminal color schemes and fonts
Support drag-and-drop workflows

5. Identity, Keys & Cryptography
This is the most important architectural change in recent versions.
Identity Model (Current)
pq-ssh now uses a single mnemonic-based identity root, from which:
A post-quantum DNA identity (Dilithium / ML-DSA-87) is derived
A classical Ed25519 SSH keypair is deterministically derived

Both are generated locally, deterministically, and without server interaction.

Files
IdentityManagerDialog.*
IdentityDerivation.*
DnaIdentityDerivation.*
OpenSshEd25519Key.*
DilithiumKeyCrypto.*
KeyMetadataUtils.*
Derivation Pipeline
4-word mnemonic (+ optional passphrase)
↓
PBKDF2-HMAC-SHA512 (2048 rounds, BIP39-compatible)
↓
64-byte master seed
↓
┌───────────────────────────┬───────────────────────────┐
│ DNA identity              │ SSH identity              │
│                           │                           │
│ SHAKE256(master || ctx)   │ SHAKE256(master || ctx)   │
│        ↓                  │        ↓                  │
│ Dilithium5 keypair        │ Ed25519 keypair           │
│        ↓                  │        ↓                  │
│ SHA3-512(pubkey)          │ OpenSSH-compatible files  │
│        ↓                  │                           │
│ DNA fingerprint           │ ~/.ssh/id_ed25519         │
└───────────────────────────┴───────────────────────────┘

Important Behavior
Dilithium keys are not used for SSH authentication
The DNA fingerprint is post-quantum secure
SSH authentication remains 100% OpenSSH-compatible
No server-side changes are required
Re-entering the same mnemonic always recreates the same identity

6. OpenSSH Config Import

Files
SshConfigParser.*
SshConfigImportDialog.*
SshConfigImportPlan.*
SshConfigImportPlanDialog.*
Responsibilities
Read and parse ~/.ssh/config safely
Preview Host blocks and defaults
Build explicit import plans
Require user confirmation

7. Remote Key Installation Workflow

User Flow
User selects a derived or generated public key
User selects a target profile
pq-ssh validates host/user/port
Application connects to the server
Ensures ~/.ssh exists with correct permissions
Backs up authorized_keys if present
Appends key only if missing
Fixes permissions
Reports result
Design Notes
Idempotent and repeatable
No server-side configuration changes
Existing keys are preserved

8. Themes & Resources

Files
ThemeInstaller.*
resources/color-schemes/
resources/docs/
pqssh_resources.qrc
Threading Model
Qt UI runs on the main thread
SSH and cryptographic work runs in background threads
Qt queued signals/slots are used exclusively
Rule
Worker threads must never manipulate UI widgets directly.
Logging & Diagnostics
Centralized logging via Logger
Explicit debug verbosity
No secrets are ever logged
Typical prefixes:
[UI] [SSH] [SHELL] [TERM] [KEYS] [DNA]

Status Summary
Implemented

Profile-based SSH connections
Embedded terminals
Profile grouping and macros
OpenSSH config import planning
Deterministic mnemonic-based identity
Post-quantum DNA fingerprint derivation
Deterministic Ed25519 SSH key derivation
Encrypted Dilithium private keys at rest
Safe public-key installation
Consistent theming and resource bundling
Explicitly Not Implemented

PQ SSH authentication
SSH agent functionality
DNA networking or resolution
Server-side PQ support