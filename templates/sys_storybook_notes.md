# Storybook / Vue Notes

Read this before writing or debugging Vue components, Storybook stories, or
play functions, or when using the Storybook MCP tools. Verified against
Storybook 10.6 (`@storybook/vue3-vite`), Vue 3.5, and the user-event version
bundled with `storybook/test` (14.6.3).

## Setting up a Vue + Storybook project from scratch

The MCP tools only exist once a dev server runs, so a cold start needs
this recipe (verified with Storybook 10.6 / Vue 3.5 / Vite 7):

1. `npm install -D vue@^3.5 storybook@^10.6 @storybook/vue3@^10.6
   @storybook/vue3-vite@^10.6 @storybook/addon-mcp@^10.6 vite@^7
   typescript @vitejs/plugin-vue`
   - `@vitejs/plugin-vue` is NOT a dependency of
     `@storybook/vue3-vite`; without it `.vue` imports fail with
     "Expression expected".
2. `vite.config.ts` must register the plugin (Storybook merges it):
   `export default defineConfig({ plugins: [vue()] });`
3. `.storybook/main.ts`: framework `@storybook/vue3-vite`, a stories
   glob, and `addons: ['@storybook/addon-mcp']` — the addon serves the
   MCP endpoint at `/mcp`.
4. Scripts: `storybook dev -p 6006` and `storybook build`.
5. Headless play tests: `npx storybook init --features test --agent
   --yes --no-dev` (installs vitest + Playwright Chromium and rewrites
   vite.config.ts). Afterwards repair what init breaks:
   - re-add `@storybook/addon-mcp` to `main.ts` addons (init drops
     existing addons);
   - delete the onboarding demo stories it copies in
     (`src/stories/{Button,Header,Page}.*`, `src/stories/assets/`);
   - add `"test": "vitest run --project=storybook"` to package.json.
6. mistral-cli wiring: `"mcpServers": { "<name>":
   { "url": "http://127.0.0.1:6006/mcp" } }` in mistral-cli.json plus
   `"mcp:*"` (or `"mcp:<name>"`) in the agent's available_tools, and
   `{ "type": "tool", "tool": "mcp:<name>" }` in policy.json's allow
   list. Connect with `/mcp connect` once the dev server runs.

## The Storybook MCP tools

- `stories-preview`, `stories-changed` and `stories-find-by-component` return
  preview URLs, story indexes and reverse-dependency graphs only. They do
  **not** run or report play-function results — a story can fail its
  interaction test while every tool call succeeds. Play results are visible
  only in the browser UI (interaction panel) or via a headless probe (below).
- Story IDs must come from the tools or the dev server's `index.json`; never
  invent them from file or export names.
- Call `get-storybook-story-instructions` before writing or editing stories.
  Imports: `Meta`/`StoryObj` from the framework package (e.g.
  `@storybook/vue3-vite`), test helpers (`expect`, `fn`, `userEvent`) from
  `storybook/test`.
- The dev server's `/mcp` endpoint (served by `@storybook/addon-mcp`) speaks
  JSON-RPC and can be probed directly to list or call tools beyond those
  wired into the current session.

## `userEvent.pointer` gotchas (user-event 14.6.3)

### Releasing a pointer button requires bracket syntax

```ts
// WRONG — silently does NOTHING:
await userEvent.pointer([
  { keys: '[MouseLeft>]', target: dial, coords: { clientX, clientY } },
  { keys: '/MouseLeft' }, // ← parsed as printable chars, dropped
]);

// CORRECT:
await userEvent.pointer([
  { keys: '[MouseLeft>]', target: dial, coords: { clientX, clientY } },
  { keys: '[/MouseLeft]' }, // ← the '/' release modifier needs brackets
]);
```

Why: in user-event's `readNextDescriptor.js`, the `/` release modifier is
only recognized inside `{...}` / `[...]` descriptors (`readTag`). A bare
`'/MouseLeft'` falls through to `readPrintableChar` and parses as the
characters `/`, `M`, `o`, ... — each looked up in the pointer key map, not
found, and **silently dropped**. No error, no `pointerup`, no `click`.

Failure signature (cost a long debugging session — don't repeat it):

- press/pointermove work, so DOM assertions pass
- anything depending on `pointerup` fails with "Number of calls: 0"
- components with window-level `pointerup` listeners stay stuck in their
  dragging state (e.g. an `is-dragging` class never clears)

### `coords` are viewport coordinates, not target-relative

`coords` are spread directly into the event init, so pass
`coords: { clientX, clientY }` (viewport coordinates). Read the target's
`getBoundingClientRect()` in the play function for deterministic drags.

### One gesture = one `userEvent.pointer` call

The default (direct) `userEvent` API creates a fresh system per top-level
call. Keep the press/move/release of a single gesture inside one
`userEvent.pointer([...])` array.

## Observing play-function failures headlessly

When a play function fails and the cause isn't obvious from the assertion
message, observe it in a real browser with a puppeteer-core probe (system
Chrome works; no need to add dependencies to the project):

- Load the story's `iframe.html?id=<storyId>&viewMode=story` directly — the
  play function autoplays there, without the manager UI.
- Inject probes before any page scripts with
  `page.evaluateOnNewDocument`: window-level pointer listeners, an
  `EventTarget.prototype.dispatchEvent` trace, and an event-constructor
  trace reveal whether events are dispatched at all and where they stop.
- The builder rewrites `storybook/test` imports to the
  `window.__STORYBOOK_MODULE_TEST__` global. Use it in-page to run
  controlled experiments with the exact `userEvent` instance the play
  function uses (including `uninstrumentedUserEvent`).
- Before assuming stale code, fetch the compiled module from the dev server
  (e.g. `curl http://localhost:<port>/src/components/Foo.vue`) and check it
  contains the expected change. HMR can leave mounted instances running old
  code — a hard browser refresh fixes that; a server restart is not needed.

## Vue component-design lessons (pointer interactions)

- **Never read `props.modelValue` back within a single gesture**
  (pointerdown → pointermove → pointerup): the parent's re-render may not
  have flushed yet, so the prop can be stale. Track the last emitted value
  in local state and emit `change` from that.
- **A readout that changes text width shifts a centered control**: an
  `inline-flex` wrapper sizes to its widest child, so value text growing
  ("50" → "100") moves a centered dial sideways while dragging. Pin the
  readout to the control's width (`:style="{ width: size + 'px' }"`,
  `text-align: center`, `white-space: nowrap`).
- **Stories that need visible two-way binding** must wrap the component in
  a small custom-`render` component with a local `ref` (v-model); args
  alone don't flow back into the rendered story.