# Security Policy

## Reporting a Vulnerability

Please **not** report security vulnerabilities through public GitHub issues. Instead, email them privately to:

**fundiman.dev@gmail.com**

To help us respond quickly, include:

- The affected version(s)
- A description of the vulnerability and its impact
- Steps to reproduce (if possible)
- Any proof-of-concept or exploit details

We aim to acknowledge reports within 48 hours and provide a fix timeline shortly after. Please give us time to fix the issue before disclosing it publicly.

## Supported Versions

| Version | Supported          |
|---------|--------------------|
| latest  | :white_check_mark: |
| older   | :x:                |

Only the latest release receives security fixes.

## Security Considerations

- File format integrity is checked via CRC32C checksums and Reed-Solomon error correction.
- If you modify the file format (`zepto.h`) or codecs, run `make test` including `corrupt_test` to verify corruption handling.
