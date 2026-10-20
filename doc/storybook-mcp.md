# Storybook MCP integration notes

Findings from exploring the Storybook MCP server with the probe script
(`examples/vue-storybook/scripts/mcp-probe.mjs`) against the Vue demo
project. This is the contract the mistral-cli MCP client (task-035/036)
implements against.

## Server

- The MCP server is the **Storybook dev server itself**: `storybook dev`
  serves the endpoint at `/mcp` via `@storybook/addon-mcp` (v10.6.2).
- `@storybook/mcp` (the npm package) is a **library**, not an executable:
  no `bin`, no stdio transport. There is nothing to spawn.
- Endpoint used in exploration: `http://127.0.0.1:6006/mcp`
  (plain HTTP/1.1 - see "Transport implications" below).

## Transport: MCP Streamable HTTP

- Client POSTs JSON-RPC 2.0 messages to the endpoint with headers:
  - `content-type: application/json`
  - `accept: application/json, text/event-stream`
  - `mcp-session-id: <id>` (after initialize; the server returns it in
    the initialize response headers)
- Observed responses:
  - `initialize` / `tools/list` / `tools/call` -> `200 OK` with
    `content-type: text/event-stream` (SSE). The JSON-RPC response
    arrives as a single SSE `data:` event; the stream closes after it.
  - `notifications/initialized` -> `202 Accepted`, empty body.
- Protocol version negotiated: `2025-06-18`.
- Capabilities: `tools` (listChanged: true), `resources` (listChanged: true).
- The `initialize` result carries an `instructions` field (agent workflow
  guidance). The client should surface it to the model alongside tool
  descriptions.

## Handshake sequence

1. `initialize` request:
   `{jsonrpc: "2.0", id: 1, method: "initialize", params: {protocolVersion: "2025-06-18", capabilities: {}, clientInfo: {name, version}}}`
   -> capture `mcp-session-id` response header.
2. `notifications/initialized` notification (no id) -> 202.
3. `tools/list` request -> tool schemas.
4. `tools/call` requests: `{method: "tools/call", params: {name, arguments}}`.

## Tool surface (v10.6.2)

Four tools - the addon exposes a story-development workflow, not the
docs-list/docs-show set from the `@storybook/mcp` library README:

| Tool | Args | Purpose |
|------|------|---------|
| `get-storybook-story-instructions` | none | Story-writing conventions (imports, play functions, mocking, naming). The server instructions say to call this before creating/editing components or stories. |
| `stories-changed` | none | Stories affected by the cumulative working-tree diff. |
| `stories-find-by-component` | `componentPaths: string[]` (required), `maxDistance?: int` (default 3) | Map component source files to story IDs via the live reverse dependency graph (Vite builder only). |
| `stories-preview` | `stories: array` (required) of `{storyId}` or `{absoluteStoryPath, exportName}`, optional `props`/`globals` | Preview URLs to hand back to the user. |

Schema notes for the C++ bridge:

- `inputSchema` uses JSON Schema draft-07 with `anyOf` unions and nested
  objects - the bridge must pass schemas through verbatim, not
  re-derive them from C++ types.
- Tools may declare `outputSchema`; responses then include a
  `structuredContent` object alongside `content`.

## Response shape of tools/call

```
result: {
  content: [ { type: "text", text: "..." } ],
  structuredContent: { ... }   // only when the tool has an outputSchema
}
```

- `content[].text` is human/model-readable; `structuredContent` is the
  machine-readable form matching `outputSchema`.
- Errors: JSON-RPC error responses carry `error: {code, message}`.

## Transport implications for the C++ client

- The vite dev server speaks plain HTTP/1.1; the wire layer is
  stunning-waddle's `http::v1::engine` via `http::client_context` with
  `protocol_version::v1_1`, driven over a plain TCP socket (the
  `address_info` resolver and the socket handling pattern in
  `lib/http/tls_connection.h` are reusable; drop the SSL layer), one
  POST per JSON-RPC message.
- Requests set `Content-Length` and `Connection: close` explicitly (the
  engine does not add them); the client reconnects for the next
  request.
- Body framing: responses observed as SSE (`text/event-stream`) that
  close after the response event; read until the SSE event whose JSON
  has the matching request id, or EOF. Also handle `application/json`
  bodies and 202-with-empty-body for notifications. The engine parses
  Content-Length, chunked, and close-delimited bodies.
- No streaming/subscription is required for the current tool set:
  every request is answered by one response. (`listChanged` capability
  notifications would arrive on a server-initiated SSE GET stream -
  not needed initially.)

## Headless play-function testing

The MCP tools return preview URLs and metadata but never report
play-function results — a story can fail its interaction test while
every tool call succeeds. The example project therefore integrates
`@storybook/addon-vitest` (vitest browser mode, Playwright Chromium):

```sh
cd examples/vue-storybook
npm test   # vitest run --project=storybook
```

Play functions run headless and failures surface as ordinary test
output — no browser inspection or puppeteer probes needed.

## Policy

MCP tool calls flow through the policy engine like any other tool.
TOOL rules named `mcp:<server>` (or `mcp:*`) match any tool registered
by that MCP server once it is connected via `/mcp connect` - the trust
decision is the deliberate act of starting the dev server and
connecting. Example rule:

```json
{ "type": "tool", "tool": "mcp:storybook" }
```

While a server is not connected, the rule matches nothing and the
default policy applies.

## Probe

```sh
cd examples/vue-storybook
npm run storybook   # in a separate terminal, keep running
node scripts/mcp-probe.mjs          # handshake + tools/list
node scripts/mcp-probe.mjs http://127.0.0.1:6006/mcp stories-find-by-component '{"componentPaths":["<abs path>"]}' --no-list
```