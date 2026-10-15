# Vue + Storybook MCP exploration project

Test project for exploring Vue application development with Storybook,
and for dogfooding the Storybook MCP server (`@storybook/mcp`) integration
in mistral-cli.

## Run

```sh
npm install
npm run storybook        # dev server on http://localhost:6006
npm run build-storybook  # static build into storybook-static/
npm test                 # headless play-function tests (vitest + chromium)
```

## Contents

- `src/components/` — demo Vue 3 components (CounterButton, TodoList, UserCard)
- `src/stories/` — CSF3 stories for each component
- `.storybook/` — Storybook configuration (vue3-vite framework)

## Storybook MCP server

The MCP server is served by the Storybook dev server itself at
`http://127.0.0.1:6006/mcp` via `@storybook/addon-mcp` (registered in
`.storybook/main.ts`). Note: `@storybook/mcp` is a library for
self-hosting the handler, not an executable — there is nothing to run
separately. See `doc/storybook-mcp.md` in the repository root for the
full contract (transport, tools, response shapes).

## Testing

Play functions run headless in Chromium via `@storybook/addon-vitest`
(vitest browser mode + Playwright):

```sh
npm test
```

The Storybook MCP tools return preview URLs and metadata, but they do
**not** report play-function results — run `npm test` for that.