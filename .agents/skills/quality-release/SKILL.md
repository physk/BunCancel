---
name: quality-release
description: Enforce reproducible CI, honest DSP metric reporting, dependency licensing and VST3 release acceptance gates.
---

# Quality and release skill

Trigger for CI, benchmarks, public releases, packaging or README performance claims.

1. Read docs/REQUIREMENTS.md, docs/TEST_PLAN.md and docs/ROADMAP.md.
2. Run generator smoke tests and eventual DSP tests across supported formats and block sizes.
3. Treat CI fixture validation as different from AEC effectiveness and host compatibility.
4. Publish comparison baselines, processing conditions, confidence and limitations.
5. Require auditable source/media licenses, toolchain versions and file checksums.
6. Verify clean Windows packaging on a fresh machine and an actual tested VST3 host.
7. Include privacy review, no personal identifiers, no unexpected recordings/uploads and no secrets.
8. Only attach success badges and performance claims for tests that actually ran.
