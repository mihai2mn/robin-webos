# Contributing to Robin

Thanks for wanting to help. The most useful contributions right now need no code at all.

## Without code

- **Tell us how Robin runs on your TV.** Robin was tested on one LG CX. A
  [Works on my TV](https://github.com/mihai2mn/robin-webos/issues/new?template=tv_report.yml) report from any other
  model, good or bad, is valuable.
- **Report sites that break**, with the [Site doesn't work](https://github.com/mihai2mn/robin-webos/issues/new?template=site_compat.yml)
  form, and attach the logs.

## With code

1. Open an issue first for anything bigger than a small fix, so we can agree on the approach.
2. Build as described in [docs/building.md](docs/building.md). A full engine build takes hours and a lot of disk; changes
   to the UI (`browser/`), the media path or the ad blocker (`engine/ubo/`) can be built on their own.
3. Test on a TV if you can, and say in the pull request what you tested and on which model. Untested changes are
   welcome too: say so.
4. Keep commits small, one change each, with a message that says what changed and why.

### House rules

- **No site names from personal browsing** in code, comments, filters, tests or commit messages. A site-specific fix
  belongs in a user's own filter list, not in the package. Widely used sites can be named in docs where it helps.
- **Plain writing** in docs and messages: say what it does, what was measured, and what is not known.
- **Keep the credit.** Robin is based on [Wren by sostk](https://github.com/sostk/webos-wpe-browser). Code that came
  from upstream stays credited.

## Licence

By contributing you agree that your contribution is licensed under GPL-3.0-or-later, the licence of this repository.

## Conduct

Be kind and assume good faith. Personal attacks, harassment and discrimination are not welcome, and the maintainer
will remove them.
