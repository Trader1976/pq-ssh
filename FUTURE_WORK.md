# pq-ssh — Future Work

This document describes ideas, experiments, and planned directions that are **not yet fully implemented** in pq-ssh.

Nothing in this file is required for the current build to function.

This file should stay future-facing. If something becomes part of the shipping product, move it into `ARCHITECTURE.md` and remove it from here.

---

## 1. Post-Quantum SSH Authentication

pq-ssh currently keeps SSH login compatible with standard OpenSSH-style authentication.
The application already experiments with PQ-safe identity roots and PQ-capable key exchange reporting, but it does **not** yet perform SSH authentication using PQ keys.

### 1.1 ML-DSA / Dilithium-style SSH authentication

Possible future work:

- define a real SSH authentication model using PQ signatures
- evaluate custom SSH key types and wire formats
- evaluate OpenSSH compatibility constraints
- determine whether this requires:
    - client-only extensions
    - server patches
    - alternative SSH implementations
- define migration paths from classical SSH keys to PQ-capable auth

### 1.2 Hybrid authentication

Possible directions:

- classical + PQ signature requirements in the same login flow
- policy-driven authentication modes
- optional fallback when server does not support PQ auth
- UI for selecting/auth-policy per profile

### 1.3 Real-world deployment model

Open questions remain:

- what is the practical meaning of “PQ SSH” in production?
- is PQ KEX enough for some deployments?
- when is PQ authentication actually worth the operational complexity?
- how much server-side change is acceptable?

---

## 2. Identity and Agent Concepts

pq-ssh already has local deterministic identity derivation, but does not yet act as an SSH agent or a full identity runtime.

### 2.1 Local PQ-aware agent

Possible future work:

- local in-memory key service
- controlled key unlock / sign requests
- passphrase-gated signing flow
- lifetime limits for unlocked keys
- profile-aware identity selection

### 2.2 Agent forwarding

Possible future work:

- evaluate agent forwarding safety model
- define whether forwarding should be allowed at all
- per-profile forwarding policy
- explicit warnings and audit logging around forwarding use

### 2.3 DNA identity integration

Open questions:

- how should mnemonic identity, PQ identity, and SSH identity relate long term?
- should DNA identity stay local-only?
- should identity export/import be formalized?
- is there any acceptable future role for network-based identity resolution?

---

## 3. Key Lifecycle and Policy

Some key metadata and expiry handling already exist, but a full policy system is not yet implemented.

Possible future work:

- automatic key rotation workflows
- stronger expiry and freshness policies
- policy enforcement by profile or group
- better surfacing of expiring / weak / legacy keys
- reusable rotation plans for many profiles
- bulk update/install flows
- stronger separation between generated, imported, and derived identities

---

## 4. UX and Workflow Improvements

pq-ssh already includes many workflow features, but there is still room for polish and simplification.

### 4.1 Connection and key workflows

Possible future work:

- guided key install wizard
- better first-run onboarding
- clearer distinction between:
    - terminal/OpenSSH path
    - SFTP/libssh path
- better presentation of negotiated KEX and auth method
- safer handling of failed profile configs

### 4.2 File and transfer UX

Possible future work:

- richer transfer logging
- transfer history view
- clearer checksum verification workflows
- better confirmations for overwrite / destructive actions
- easier remote/local comparison tools

### 4.3 Profiles and organization

Possible future work:

- colored profile groups
- richer group-level defaults
- saved filters / search / profile tagging
- profile health indicators
- profile-specific warnings or trust markers

### 4.4 Tooltips and guidance

Possible future work:

- more targeted tooltips
- inline explanations for advanced SSH options
- clearer warnings around experimental PQ-related settings
- better diagnostics text for common failures

---

## 5. Security Hardening

This section covers implementation hardening work, not feature expansion.

Possible future work:

- broader memory wiping and lifetime review
- stronger handling of sensitive buffers in more code paths
- side-channel review for local cryptographic operations
- stronger passphrase handling guarantees
- audit of child-process argument exposure risks
- threat model documentation
- packaging and supply-chain hardening notes
- release hardening checklist

---

## 6. Terminal, Transport, and KEX Evolution

pq-ssh currently uses two separate transport paths:

- OpenSSH for the interactive terminal
- libssh for SFTP and helper operations

This is intentional, but it creates future design questions.

Possible future work:

- align KEX policy presentation across OpenSSH and libssh paths
- detect and explain when terminal and SFTP negotiate different algorithms
- evaluate whether terminal path should prefer ML-KEM when host OpenSSH supports it
- decide how aggressively to expose KEX policy controls in UI
- add clearer transport-capability diagnostics to logs and status badges

---

## 7. Fleet, Scheduling, and Automation

Fleet jobs and scheduled jobs exist, but there is still room to mature them.

Possible future work:

- richer scheduling policies
- job dependencies and retry policy
- stronger audit integration for scheduled execution
- better dry-run support
- profile group targeting
- safer bulk execution confirmations
- export/import of job definitions

---

## 8. Packaging and Release Engineering

pq-ssh now has AppImage packaging, but distribution work is not “done forever”.

Possible future work:

- cleaner automatic version extraction for release builds
- release artifact naming without manual rename steps
- automatic SHA256 artifact generation in release workflow
- multi-platform packaging strategy
- improved Linux desktop integration guidance
- reproducibility notes for private dependency builds
- clearer packaging docs for bundled libssh and host OpenSSH interaction

---

## 9. Open Questions

These remain intentionally undecided:

- What should “PQ SSH client” mean in real deployments?
- Should pq-ssh remain strictly OpenSSH-compatible for login forever?
- Is server modification acceptable for future PQ auth experiments?
- How much identity functionality should remain local-only?
- Should the app ever expose more raw crypto choices to end users?
- How much should be automated vs explicitly confirmed in security-sensitive workflows?

---

## 10. Other Ideas

Smaller or exploratory ideas worth revisiting later:

- real Dilithium / ML-DSA key generation workflows for non-SSH use cases
- richer audit views and filters
- better destructive-action confirmation design
- more transfer diagnostics
- easier checksum verification UX
- more settings polish and organization
- advanced tooltips and inline docs
- profile badges / trust states / warning markers

---

## Keep This File Honest

When editing this document:

- remove items that are already implemented
- avoid mixing current architecture with future ideas
- prefer concrete open questions over vague marketing language
- link shipping behavior back to `ARCHITECTURE.md`, not here