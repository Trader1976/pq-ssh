
<img width="983" height="603" alt="macros" src="https://github.com/user-attachments/assets/089ce439-3cf8-441c-a5e4-b2449b3bb184" />
<img width="1199" height="686" alt="filesystem" src="https://github.com/user-attachments/assets/18d7772c-2407-4990-8b0c-51d64a2c1c91" />


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
