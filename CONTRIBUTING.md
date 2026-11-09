# Contributing to xkeys

xkeys uses pull requests to record design decisions, implementation, validation
and acceptance.

## Working agreement

- `master` holds accepted, runnable history. Keep feature commits on a separate
  branch.
- Use one branch per agreed phase or task. Example names include
  `codex/phase-1-build-baseline` and `codex/fix/editor-focus`.
- Run the checks in [the development baseline](docs/development-baseline.md)
  and any additional checks specific to the change.
- Record the engineering gate with exact commands, actual results and known
  limitations. Record hands-on acceptance separately with steps, observations
  and the maintainer's decision. Both gates matter for phase acceptance.
- Keep local plans, logs, personal profiles, captures, credentials and build
  outputs out of commits. Use public task descriptions and sanitised evidence
  in the pull request. The root `PLAN.md` is private and ignored.
- Prepare the branch for local review. Obtain the maintainer's authorization
  before pushing a branch, opening a pull request or merging into `master`.
- Put review fixes on the same branch and pull request. Obtain explicit
  authorization before force-pushing.

## Typical flow

1. Start an agreed branch from current `master`.
2. Make the scoped changes and run the engineering checks.
3. Review the diff and commit the reviewed files on the branch.
4. Once publication is authorized, push the branch and open a pull request using
   the repository template.
5. Record the maintainer's hands-on review separately from the engineering results.
   The maintainer decides whether to merge, request changes or close the pull request.
