<img width="983" height="603" alt="macros" src="https://github.com/user-attachments/assets/089ce439-3cf8-441c-a5e4-b2449b3bb184" />
<img width="1199" height="686" alt="filesystem" src="https://github.com/user-attachments/assets/18d7772c-2407-4990-8b0c-51d64a2c1c91" />

# PQ-SSH — Post-Quantum Secure Shell

PQ-SSH is a Qt/C++ desktop SSH client that combines practical OpenSSH compatibility with post-quantum experimentation.

It is **not** a new SSH protocol or a server-side replacement.  
Instead, it is a desktop client that currently uses:

- **OpenSSH** for the interactive terminal
- **libssh** for SFTP and helper operations
- **deterministic mnemonic-based identity derivation**
- **post-quantum identity / fingerprint experiments**
- **standard OpenSSH-compatible SSH authentication**

---

## Current Status

PQ-SSH is now usable as a real desktop SSH client, with active work continuing around:

- profile-based SSH connections
- embedded terminal sessions
- SFTP and remote file operations
- profile grouping and macros
- OpenSSH config import
- deterministic identity recovery
- post-quantum identity experiments
- Linux AppImage packaging

Important:

- PQ-SSH currently **does not** use post-quantum keys directly for SSH login
- SSH authentication remains **OpenSSH-compatible**
- no server-side changes are required for normal use

---

## What PQ-SSH Does Today

### Interactive terminal
- Embedded terminal UI via **QTermWidget**
- Interactive shell sessions launched through the system's **OpenSSH `ssh`**
- Profile-based connection launch
- Terminal tabs or separate windows
- Terminal macros and shortcuts

### Remote file operations
- SFTP-backed file browsing
- Upload and download
- Remote directory listing and stat
- Safer temp-file and replace flows
- Transfer-related logging and status reporting

### Profiles and workflow
- GUI-based profile editor
- Groups and macros
- Port-forwarding settings
- OpenSSH `~/.ssh/config` import planning
- Scheduled jobs and fleet-oriented workflow groundwork

### Identity and crypto
- 24-word mnemonic-based identity recovery
- Deterministic identity derivation
- Post-quantum identity branch and fingerprinting
- Deterministic Ed25519 SSH identity derivation
- OpenSSH-compatible SSH key material for login
- Encrypted private material handling where applicable

### Packaging
- Linux AppImage distribution
- Private bundled libssh in the AppImage
- No system installation required for the packaged client itself

---

## Current Architecture in One Paragraph

PQ-SSH uses two separate SSH paths by design:

- the **interactive terminal path** uses the host system's OpenSSH client
- the **programmatic path** uses **libssh** for SFTP and helper commands

This split keeps terminal behavior highly compatible while still allowing the application to provide file management and structured SSH workflows inside the GUI.

For more detail, see `ARCHITECTURE.md`.

---

## Post-Quantum Status

PQ-SSH includes real post-quantum-oriented work, but it is important to be precise about what that means today.

### Implemented today
- PQ-capable identity experiments
- PQ-style deterministic identity derivation
- PQ fingerprint / identity-root workflows
- PQ-capable KEX reporting where supported by transport/backend
- libssh 0.12 packaging and use in the AppImage

### Not implemented today
- PQ SSH authentication
- Dilithium / ML-DSA login to normal SSH servers
- server-side PQ deployment
- SSH agent replacement
- decentralized identity networking

In other words:

**PQ-SSH is currently a practical SSH client with PQ experiments and PQ-capable building blocks, not a fully PQ-authenticated SSH ecosystem yet.**

---

## Features

### Implemented
- Profile-based SSH connections
- Embedded terminal sessions
- SFTP file operations
- Profile groups
- Macros and shortcuts
- OpenSSH config import planning
- Deterministic mnemonic-based identity
- Deterministic Ed25519 SSH key derivation
- Post-quantum identity derivation experiments
- Public-key installation into remote `authorized_keys`
- Theme support
- Logging and audit support
- Linux AppImage packaging

### Planned / Future Work
- PQ SSH authentication experiments
- hybrid authentication models
- richer policy enforcement
- stronger key lifecycle tooling
- deeper security hardening
- broader release automation

See `FUTURE_WORK.md` for forward-looking ideas.

---

## Linux AppImage

PQ-SSH is distributed on Linux as an AppImage.

Typical usage:

    chmod +x CPUNK-PQ-SSH-1.0-x86_64.AppImage
    ./CPUNK-PQ-SSH-1.0-x86_64.AppImage

Notes:

- the AppImage bundles the client and its private libssh dependency
- the interactive terminal path still uses the host system's OpenSSH client
- desktop integration depends on your Linux environment

---

## Build Notes

PQ-SSH is a C++17 / Qt Widgets project.

Main technologies include:

- Qt Widgets
- libssh
- QTermWidget
- libsodium
- vendored PQ crypto components used for current identity experiments

See:

- `ARCHITECTURE.md`
- `FUTURE_WORK.md`
- `CMakeLists.txt`

---

## Project Positioning

PQ-SSH should currently be understood as:

- a desktop SSH client
- with an embedded terminal
- with SFTP and remote file workflows
- with deterministic mnemonic-based identity recovery
- with post-quantum identity experiments
- with standard OpenSSH-compatible authentication

It is **not** currently a drop-in full replacement for the whole SSH ecosystem.

---

## License

This project is licensed under the Apache License 2.0.

Commercial licenses are available for enterprise and OEM use.

See:

- `LICENSE`
- `DUAL_LICENSE.md`

---

## Repository Docs

- `ARCHITECTURE.md` — current implemented architecture
- `FUTURE_WORK.md` — ideas and directions not yet implemented

---

## Cryptography Notes

PQ-SSH currently uses:

- **libsodium** for modern password-based key derivation and authenticated encryption workflows
- deterministic seed derivation for identity workflows
- OpenSSH-compatible key formats where SSH interoperability is required

Post-quantum identity work exists in the client, but SSH login itself remains classical/OpenSSH-compatible today.

---

## Summary

PQ-SSH is a practical desktop SSH client with:

- real terminal and SFTP usability
- modern profile-driven workflows
- deterministic identity recovery
- post-quantum experimentation where it is currently practical
- honest compatibility with today's SSH reality
