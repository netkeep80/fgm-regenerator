# Requirements hierarchy

**Canonical source:** `requirements/*.rg.yaml` (normative text and lifecycle). README and this page are explanations, not independent normative authority.

```text
FGMR-PRODUCT-000   Product capability root
FGMR-PROJECT-000   Project and evidence root
  ├── FGMR-GOV-001   RepoGuard immutable BASE/HEAD integration
  └── FGMR-LIC-001   License and asset boundary
```

Product and project requirements are deliberately separate. `derives_from` gives the decomposition, `implemented_by` identifies an actual implementing resource, `verified_by` identifies an executable verification resource, and `documents` is a documentation link only. At bootstrap, the only claimed implementation is the RepoGuard workflow; no product feature or functional test is claimed.

RepoGuard v6.0.1 is pinned to commit `74db97791e6da8a75287164f0d0b41579108f114`. A full transition check compares the exact PR BASE→HEAD and is not equivalent to HEAD→HEAD. GitHub Actions itself does not enable branch protection; an administrator must set the `RepoGuard exact BASE/HEAD` job as required.
