// Headless-Chrome diagnostic for the MouseDragAdjust play function.
//
// Loads the story's iframe.html directly (the play function autoplays there),
// with a probe injected before any page scripts run. The probe logs:
//   - every window.addEventListener('pointer...') registration
//   - every pointer event that reaches the window
// plus all console output (including the play function's assertion errors).
//
// Usage:
//   node scripts/play-probe.mjs

import puppeteer from 'puppeteer-core';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const STORY_URL =
  'http://localhost:6006/iframe.html?id=components-knob--mouse-drag-adjust&viewMode=story';

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: 'new',
  args: ['--no-sandbox', '--disable-dev-shm-usage'],
});

const page = await browser.newPage();
await page.setViewport({ width: 1400, height: 900 });

const logs = [];
page.on('console', (msg) => {
  logs.push(`[console.${msg.type()}] ${msg.text()}`);
});
page.on('pageerror', (err) => {
  logs.push(`[pageerror] ${err.message}`);
});

// Injected into every frame before any of its scripts run.
await page.evaluateOnNewDocument(() => {
  if (window.__probeInstalled) return;
  window.__probeInstalled = true;
  const log = (...a) => console.log('[probe]', ...a);
  const origAdd = window.addEventListener.bind(window);
  window.addEventListener = function (type, fn, opts) {
    if (typeof type === 'string' && type.startsWith('pointer')) {
      log('register', type);
    }
    return origAdd(type, fn, opts);
  };
  origAdd('pointerdown', (e) => log('evt window pointerdown', e.clientX, e.clientY));
  origAdd('pointermove', (e) => log('evt window pointermove', e.clientX, e.clientY));
  origAdd('pointerup', (e) => log('evt window pointerup', e.clientX, e.clientY));

  // Trace every dispatched pointer/mouse event: type, target, flags.
  const describe = (t) =>
    t === window ? 'window' : t === document ? 'document' : `${t.tagName ?? '?'}${t.id ? '#' + t.id : ''}${t.className && typeof t.className === 'string' ? '.' + t.className.split(' ').join('.') : ''}`;
  const origDispatch = EventTarget.prototype.dispatchEvent;
  EventTarget.prototype.dispatchEvent = function (event) {
    if (typeof event?.type === 'string' && (event.type.startsWith('pointer') || event.type === 'click')) {
      log(
        'dispatch', event.type,
        'target=', describe(this),
        'connected=', this instanceof Node ? this.isConnected : 'n/a',
        'bubbles=', event.bubbles,
        'defaultPrevented=', event.defaultPrevented
      );
    }
    return origDispatch.call(this, event);
  };
  // Trace PointerEvent constructions.
  const OrigPE = window.PointerEvent;
  window.PointerEvent = class extends OrigPE {
    constructor(type, init) {
      super(type, init);
      log('new PointerEvent', type);
    }
  };

  log('installed in', location.href);
});

await page.goto(STORY_URL, { waitUntil: 'domcontentloaded', timeout: 60000 });

// Give the story time to render and the play function time to run.
await new Promise((r) => setTimeout(r, 20000));

// Controlled experiments with the exact userEvent instance the play uses.
try {
  const experiment = await page.evaluate(async () => {
    const out = [];
    const say = (m) => out.push(m);
    const mod = window.__STORYBOOK_MODULE_TEST__;
    say(`module keys: ${mod ? Object.keys(mod).length : 'MISSING'}`);
    const dial = document.querySelector('[role="slider"]');
    if (!mod || !dial) {
      say('missing module or dial, aborting experiments');
      return out;
    }

    say('--- experiment 1: instrumented userEvent press+release ---');
    try {
      await mod.userEvent.pointer([
        { keys: '[MouseLeft>]', target: dial, coords: { clientX: 10, clientY: 10 } },
        { keys: '/MouseLeft' },
      ]);
      say('experiment 1 resolved');
    } catch (e) {
      say(`experiment 1 rejected: ${e.message}`);
    }

    say('--- experiment 2: uninstrumentedUserEvent press+release ---');
    try {
      await mod.uninstrumentedUserEvent.pointer([
        { keys: '[MouseLeft>]', target: dial, coords: { clientX: 10, clientY: 10 } },
        { keys: '/MouseLeft' },
      ]);
      say('experiment 2 resolved');
    } catch (e) {
      say(`experiment 2 rejected: ${e.message}`);
    }

    say('--- experiment 3: uninstrumentedUserEvent.setup() press+release ---');
    try {
      const ue = mod.uninstrumentedUserEvent.setup();
      await ue.pointer([
        { keys: '[MouseLeft>]', target: dial, coords: { clientX: 10, clientY: 10 } },
        { keys: '/MouseLeft' },
      ]);
      say('experiment 3 resolved');
    } catch (e) {
      say(`experiment 3 rejected: ${e.message}`);
    }

    say('--- experiment 4: release with bracket syntax [/MouseLeft] ---');
    try {
      await mod.uninstrumentedUserEvent.pointer([
        { keys: '[MouseLeft>]', target: dial, coords: { clientX: 10, clientY: 10 } },
        { keys: '[/MouseLeft]' },
      ]);
      say('experiment 4 resolved');
    } catch (e) {
      say(`experiment 4 rejected: ${e.message}`);
    }

    return out;
  });
  console.log('==== EXPERIMENTS ====');
  for (const l of experiment) console.log(l);
} catch (e) {
  console.log('==== EXPERIMENTS (failed) ====');
  console.log(e.message);
}

// Dump the final DOM state of the story.
try {
  const state = await page.evaluate(() => {
    const dial = document.querySelector('[role="slider"]');
    return {
      readout: document.querySelector('.knob-readout')?.textContent ?? null,
      dialExists: !!dial,
      dialClass: dial?.className ?? null,
      ariaValuenow: dial?.getAttribute('aria-valuenow') ?? null,
    };
  });
  console.log('==== FINAL STATE ====');
  console.log(JSON.stringify(state, null, 2));
} catch (e) {
  console.log('==== FINAL STATE (failed) ====');
  console.log(e.message);
}

console.log('==== CAPTURED LOGS ====');
for (const l of logs) {
  console.log(l);
}

await browser.close();