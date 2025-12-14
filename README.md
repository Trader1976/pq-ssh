# PQ-SSH — Post-Quantum Secure Shell

PQ-SSH is a post-quantum secure replacement for traditional SSH, designed to
resist both classical and quantum cryptographic attacks.

## Features (Planned)
- Post-quantum key exchange (Kyber family)
- Post-quantum authentication (Dilithium family)
- Encrypted terminal access
- Secure file transfer
- Secure port forwarding
- Qt-based GUI client

## License

This project is licensed under the Apache License 2.0.

Commercial licenses are available for enterprise and OEM use.


## Cryptography

PQ-SSH uses libsodium for password-based key derivation (Argon2id) and
authenticated encryption of post-quantum private keys.