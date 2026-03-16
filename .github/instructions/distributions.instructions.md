---
description: 'WSL distribution metadata validation'
applyTo: 'distributions/**'
---

# Distributions — WSL

## Distribution Validation

- Validate metadata: `python3 distributions/validate.py distributions/DistributionInfo.json`
- Validate modern distributions: `python3 distributions/validate-modern.py`
- **Note**: May fail on Linux due to network restrictions accessing distribution URLs.

## CI/CD

- **distributions.yml** (GitHub Actions): Validates distribution metadata on Linux
- **modern-distributions.yml** (GitHub Actions): Tests modern distribution support

## After Changes

1. Run `python3 distributions/validate.py distributions/DistributionInfo.json`
2. If adding a new distribution, test with actual WSL distribution installation on Windows
