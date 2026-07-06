# Git Strategy

## Branching
- `main` is always deployable.
- Work happens on short-lived feature branches: `feature/<topic>`.
- Hotfixes use `hotfix/<issue>`.

## Pull Requests
- Require CI green.
- At least one review for core modules.
- Squash merge preferred to keep history clean.

## Commit Messages
Format: `<scope>: <summary>`

Examples:
- `network: add WSAPoll socket manager`
- `engine: implement order matching`
- `docs: update architecture notes`
