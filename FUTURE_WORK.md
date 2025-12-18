# pq-ssh — Future Work

This document describes **ideas, experiments, and planned directions**
that are **not yet implemented** in pq-ssh.

Nothing in this file is required for the current build to function.

---

## 1. Post-Quantum SSH Authentication

### 1.1 Dilithium-based SSH auth
- Server-side support requirements
- Custom SSH key types
- Compatibility constraints

### 1.2 Hybrid authentication
- Classical + PQ signatures
- Policy-driven authentication

---

## 2. Identity & Agent Concepts

### 2.1 Local PQ agent
### 2.2 Agent forwarding
### 2.3 DNA identity integration (open questions)

---

## 3. Key Lifecycle & Policy

- Automatic rotation
- Policy enforcement
- Expiry warnings

---

## 4. UX & Workflow Improvements

- Key install wizards
- Audit views
- Safer destructive actions

---

## 5. Security Hardening

- Memory wiping
- Side-channel considerations
- Threat model documentation

---

## 6. Open Questions

- What does “PQ SSH” mean in real deployments?
- How much server change is acceptable?


# 7. Other ideas

- Profiles grouping (colors)
- Dilithium keygen (real)
- tooltips
- logging on file transfers
- confirm checksum calculation
- settings menu
- 
