// Minimal MCP Streamable HTTP probe for the Storybook dev server's /mcp
// endpoint (served by @storybook/addon-mcp). Performs the initialize
// handshake, lists tools, and optionally calls one.
//
// Usage:
//   node scripts/mcp-probe.mjs [url] [toolName] [argsJson]
//
// Examples:
//   node scripts/mcp-probe.mjs
//   node scripts/mcp-probe.mjs http://127.0.0.1:6006/mcp docs-list '{}'

const endpoint = process.argv[2] ?? 'http://127.0.0.1:6006/mcp';
const callTool = process.argv[3] ?? null;
const callArgs = process.argv[4] ? JSON.parse(process.argv[4]) : {};
const noList = process.argv.includes('--no-list');

let sessionId = null;

async function post(message) {
  const headers = {
    'content-type': 'application/json',
    accept: 'application/json, text/event-stream',
  };
  if (sessionId) {
    headers['mcp-session-id'] = sessionId;
  }
  const res = await fetch(endpoint, {
    method: 'POST',
    headers,
    body: JSON.stringify(message),
  });
  const newSession = res.headers.get('mcp-session-id');
  if (newSession) {
    sessionId = newSession;
  }
  return res;
}

// Reads a POST response and extracts JSON-RPC messages. Handles both
// plain application/json responses and text/event-stream responses
// (the Streamable HTTP transport allows either).
async function readMessages(res) {
  const contentType = res.headers.get('content-type') ?? '';
  const text = await res.text();

  if (contentType.includes('text/event-stream')) {
    const messages = [];
    for (const block of text.split('\n\n')) {
      const data = block
        .split('\n')
        .filter((line) => line.startsWith('data:'))
        .map((line) => line.slice(5).trim())
        .join('');
      if (data) {
        try {
          messages.push(JSON.parse(data));
        } catch {
          // ignore malformed keep-alives
        }
      }
    }
    return messages;
  }

  if (!text) {
    return []; // e.g. 202 Accepted for notifications
  }
  try {
    return [JSON.parse(text)];
  } catch {
    return [];
  }
}

function show(label, res, messages) {
  console.log(`--- ${label} ---`);
  console.log(`status: ${res.status} ${res.statusText}`);
  console.log(`content-type: ${res.headers.get('content-type')}`);
  if (sessionId) {
    console.log(`mcp-session-id: ${sessionId}`);
  }
  for (const message of messages) {
    console.log(JSON.stringify(message, null, 2));
  }
  console.log('');
}

// 1. Initialize handshake
const initRes = await post({
  jsonrpc: '2.0',
  id: 1,
  method: 'initialize',
  params: {
    protocolVersion: '2025-06-18',
    capabilities: {},
    clientInfo: { name: 'mistral-cli-probe', version: '0.1.0' },
  },
});
const initMessages = await readMessages(initRes);
show('initialize', initRes, initMessages);

const serverInfo = initMessages[0]?.result?.serverInfo;
if (!serverInfo) {
  console.error('initialize failed, aborting');
  process.exit(1);
}

// 2. Initialized notification
const notifRes = await post({
  jsonrpc: '2.0',
  method: 'notifications/initialized',
});
await readMessages(notifRes);
console.log('--- notifications/initialized ---');
console.log(`status: ${notifRes.status} (session: ${sessionId ?? 'none'})`);
console.log('');

// 3. List tools
if (!noList) {
  const listRes = await post({
    jsonrpc: '2.0',
    id: 2,
    method: 'tools/list',
  });
  const listMessages = await readMessages(listRes);
  show('tools/list', listRes, listMessages);
}

// 4. Optionally call a tool
if (callTool) {
  const callRes = await post({
    jsonrpc: '2.0',
    id: 3,
    method: 'tools/call',
    params: { name: callTool, arguments: callArgs },
  });
  const callMessages = await readMessages(callRes);
  show(`tools/call ${callTool}`, callRes, callMessages);
}
