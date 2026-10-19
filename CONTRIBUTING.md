# Contributing to orbis-reframe

Thanks for your interest. orbis-reframe is a small project run by one
person, so the best way to start is an issue:

- **Found a bug?** Open a [bug report](https://github.com/Schabo/orbis-reframe/issues/new/choose).
  Include the diagnostics from **Help ▸ About orbis-reframe ▸ Copy
  diagnostics**.
- **Want a feature, or want to work on one?** Open an issue first and
  describe it, so we can agree on the approach before you spend time on a
  pull request.
- **Small fixes** (typos, docs, an obvious bug) can go straight to a pull
  request.
- **Have a 360 camera other than the X5?** Short sample clips straight from
  its card are a big help: see
  [Sample clips wanted](docs/formats/README.md#sample-clips-wanted).

## Building and testing

You need Debian Testing (or the same library versions), a GPU with Vulkan
(NVIDIA is what the maintainer tests on; AMD and Intel on Mesa work
best-effort), and some Insta360 X5 `.insv` clips to try things on. The clips
aren't in the repository.

- [`docs/dev-environment.md`](docs/dev-environment.md) lists the packages,
  first-time setup, and the environment switches for diagnosing problems.
- `./scripts/build.sh` configures, builds and runs the tests (`ctest`);
  `./scripts/build.sh -r` also starts the app.
- [`tools/README.md`](tools/README.md) describes the command-line tools for
  checking stitching, playback, stabilization, tracking, the privacy blur and
  audio filters on real clips.

A pull request should build with `./scripts/build.sh` and pass its tests.
Say in the description which clips and hardware you tried it on.

GitHub Actions (`.github/workflows/ci.yml`) builds every pull request that
changes more than docs on Debian Testing, Debian 13 and Ubuntu 26.04 with
every feature on, and runs the tests. It has no GPU, so it can't replace
trying the change on real clips. `.github/workflows/commits.yml` checks the
commit messages of every pull request, docs included. For contributors from
outside the project these runs start once a maintainer has approved them.
`.github/workflows/gpu.yml` runs the GPU checks (build, tests, the
headless GPU tools and a smoke test of the whole app on a real clip) on the
maintainer's machine, for pushes to
`main` and for pull requests that change more than docs. For a pull request
from a fork it waits for a second approval, since it runs your code on that
machine.

## Commit messages

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/).
The version number and `CHANGELOG.md` are made from them, so the format
matters:

```
type(scope): what changed, in the imperative or as a statement

Optional body: why, and anything a reviewer should know.
```

- **Types:** `feat` (new user-visible feature), `fix` (bug fix), `perf`,
  `refactor`, `docs`, `test`, `build`, `ci`, `chore`. Only `feat` and `fix`
  (and breaking changes) change the version and appear in the changelog.
- **Scope** (optional) is the area: `privacy`, `playback`, `stitch`, `stab`,
  `audio`, `timeline`, `export`, `track`, `ui`, `insv`, `core`, `render`,
  `build`.
- A change that breaks existing project files (`.orp`) needs a
  `BREAKING CHANGE:` line in the body.
- Don't edit the version number by hand; the release process sets it.

A commit hook checks the format with commitizen. Turn it on once per clone:

```sh
sudo apt install commitizen
git config core.hooksPath .githooks
```

## Code and docs

- Match the code around your change: naming, formatting, comment style.
  Comments explain *why*, and stand on their own.
- Keep the docs in step with the change:
  - the user manual ([`docs/manual/`](docs/manual/README.md)) for anything
    users see: menus, panels, shortcuts, dependencies;
  - [`tools/README.md`](tools/README.md) when a tool is added, removed or
    changes;
  - [`docs/dev-environment.md`](docs/dev-environment.md) when build
    dependencies change;
  - the camera's document in [`docs/formats/`](docs/formats/README.md) for
    new findings about a file format, with where they come from.

## Camera file formats: interoperability only

Everything orbis-reframe knows about the cameras' files comes from
interoperability documentation, published research, and files the cameras
recorded. **Don't disassemble or decompile a camera vendor's SDK or
software** (Insta360's, DJI's or anyone's), and don't contribute anything you
derived from that. Facts that other open-source projects found that way are
usable only as file-format facts, cited and checked against real files; the
vendor's own tuning is measured, not copied. The rules are in
[`docs/formats/README.md`](docs/formats/README.md). Each document there records
where each fact comes from; keep doing that for new ones.

## License of contributions

By contributing, you agree that your contribution is licensed under the
project's licenses:

- the application: [GNU General Public License v3.0 or later](LICENSE);
- the `.insv` library in [`src/insv/`](src/insv/): MIT OR Apache-2.0, at
  the user's option ([`LICENSE-MIT`](src/insv/LICENSE-MIT),
  [`LICENSE-APACHE`](src/insv/LICENSE-APACHE)).

Third-party code or models need a license compatible with these. Add them
to [`THIRD_PARTY.md`](THIRD_PARTY.md) with their license text.
