# The manual of the JS developer of qjsx

A site made with [Astro](https://astro.build) and [Starlight](https://starlight.astro.build):

- the guides are written by hand, in `src/content/docs/guides/`;
- the API reference (`src/content/docs/api/`, not in git) is generated at each build by
  [starlight-typedoc](https://github.com/HiDeoo/starlight-typedoc) from the typings of
  qjsx: one `.d.ts` next to each module of the repository, listed in
  `tsconfig.typedoc.json` and in `astro.config.mjs` (a new module goes in both);
  `api-readme.md` is its introduction.

```bash
npm install        # once
npm run dev        # http://localhost:4321, reloads on each change
npm run build      # the static site in dist/
```
