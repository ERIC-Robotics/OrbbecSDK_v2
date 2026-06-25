# Contributing Guidelines

## Git Workflow
- Work on short-lived **feature branches** from `dev`.
  - `feat/<feature-name>`
  - `fix/<bug-name>`
- Push daily to remote branches.  
  - If incomplete, prefix commit with `WIP:`.
- Open a **Pull Request (PR)** to `dev`.
- **Do not commit directly** to `dev` or `main`.

## Commit Messages
Follow [Conventional Commits](https://www.conventionalcommits.org/):
- `feat: add ROS data logger`
- `fix: correct preprocessing bug`

## Reviews
- Every PR requires at least **1 reviewer approval**.
- Use **Draft PRs** for work-in-progress.

## Tests
- Run tests before opening PR.
- CI must pass before merge.