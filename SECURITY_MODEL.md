pq-ssh Security Model

This document describes the security assumptions, guarantees, and non-goals of pq-ssh as it is currently implemented.

It is intended for users, contributors, and reviewers who want to understand what pq-ssh does and does not protect, and where trust boundaries exist.

This document does not describe future plans, experimental ideas, or speculative cryptography.

Scope and Purpose

pq-ssh is a desktop SSH client.
Its primary responsibility is to safely initiate SSH connections, manage SSH profiles, and handle SSH keys on the client side, while delegating cryptographic protocol security to well-established libraries.

pq-ssh does not attempt to replace OpenSSH, the SSH protocol, or the operating system’s security model.

Security Goals

pq-ssh is designed to achieve the following security goals:

Protect SSH private keys stored on disk from accidental exposure.
Avoid modifying remote server security state beyond explicit user-approved actions.
Ensure that SSH connections are created using well-defined, predictable parameters.
Prevent accidental key duplication or corruption during installation.
Avoid leaking credentials, private keys, or secrets through logs or UI.
Provide deterministic and inspectable behavior for profile-based connections.

Out of Scope

The following are explicitly out of scope for pq-ssh:

Protection against malware on the local system.
Protection against a compromised operating system or kernel.
Protection against malicious or compromised SSH servers.
Protection against users copying or exporting keys manually.
Replacing SSH protocol security guarantees.
Providing anonymity, federation, or identity attestation.
Acting as an SSH agent or key agent.

If the local system is compromised, pq-ssh cannot provide meaningful security guarantees.

Threat Model

pq-ssh considers the following threat classes:

A passive network attacker observing traffic.
A remote SSH server that may behave incorrectly or maliciously.
Accidental user error during key installation or profile editing.

pq-ssh does not attempt to defend against:

An attacker with full local filesystem access.
An attacker controlling the local runtime environment.
An attacker modifying pq-ssh binaries or libraries.

Trust Boundaries

pq-ssh establishes clear trust boundaries:

pq-ssh trusts the operating system for filesystem permissions, process isolation, and randomness.
pq-ssh trusts libssh to correctly implement the SSH protocol.
pq-ssh trusts qtermwidget for terminal emulation correctness.
pq-ssh does not trust remote SSH servers.
pq-ssh does not trust contents of ~/.ssh/config without user review.
pq-ssh does not trust imported data without validation.

All trust boundaries are explicit and conservative.

SSH Protocol Security

pq-ssh does not modify the SSH protocol.

All SSH authentication, key exchange, encryption, and integrity guarantees are provided by libssh and the SSH protocol itself.

pq-ssh uses only OpenSSH-compatible authentication mechanisms when interacting with servers.

No custom authentication extensions are used.

Key Handling Model

pq-ssh handles SSH keys as follows:

Private keys are never transmitted to remote servers.
Private keys are never logged.
Public keys are derived and displayed explicitly.
Public keys are installed only with explicit user approval.

When keys are written to disk, pq-ssh ensures file permissions are restricted to the owning user.

pq-ssh does not attempt to hide keys from the user or enforce key usage policies.

Remote Key Installation Safety

When installing a public key on a remote server, pq-ssh follows a conservative and idempotent process:

The ~/.ssh directory is created if missing.
Permissions are explicitly set to secure defaults.
Existing authorized_keys files are preserved and backed up.
Public keys are appended only if they are not already present.
No server configuration files other than authorized_keys are modified.

This process is designed to be repeatable and safe to run multiple times.

Configuration Import Safety

When importing OpenSSH configuration:

Files are parsed in read-only mode.
No OpenSSH configuration files are modified.
Wildcard hosts and invalid values are handled defensively.
The user must explicitly approve all profile creation or updates.

pq-ssh treats imported configuration as untrusted input.

Logging and Diagnostics

pq-ssh provides logging for debugging and support purposes.

The following rules apply:

Passwords are never logged.
Private key material is never logged.
Derived secrets are never logged.
Verbose logging must be explicitly enabled.

Log files are stored locally and subject to the operating system’s file permissions.

User Responsibility

Users are responsible for:

Protecting their local system.
Choosing strong passwords and passphrases.
Reviewing keys before installation.
Managing backups of their keys and profiles.

pq-ssh does not enforce operational security policies.

Cryptographic Choices (High-Level)

pq-ssh relies on well-established cryptographic libraries rather than custom implementations.

Cryptographic primitives are selected for correctness, availability, and auditability rather than novelty.

All cryptographic operations are performed locally.

Future Work Disclaimer

This security model applies only to the current implementation.

Any future features that change authentication mechanisms, identity models, or trust boundaries must update this document accordingly.

Summary

pq-ssh is a client-side SSH tool with conservative security goals.

It prioritizes clarity, explicit user control, and predictable behavior over abstraction or automation.

It does not attempt to redefine SSH security and makes no claims beyond its defined scope.