"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {test} = require("node:test");

async function app() {
  const elements = Object.create(null), calls = [], packets = [];
  for (const id of ["editor", "message", "status", "fasMode", "fasMarginBase", "fasOverrides", "fallbackFps", "sceneAux", "sceneGpuLock", "save", "refresh", "rollback"])
    elements[id] = {value: "", textContent: "", placeholder: "", dataset: {}, listeners: {},
      addEventListener(name, callback) { this.listeners[name] = callback; }};
  const seed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/seed.json"), "utf8"));
  const sceneSeed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/scene-seed.json"), "utf8"));
  let settings = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/settings.json"), "utf8"));
  settings.sceneMode = "performance";
  delete settings.fasMode; // Existing installed settings must remain readable.
  let context;
  const window = {ksu: {exec(command, _options, callback) {
    calls.push(command);
    let output = "", error = "", code = 0;
    if (command.endsWith(" scene-seed")) output = JSON.stringify(sceneSeed);
    else if (command.endsWith(" aux-seed")) output = fs.readFileSync(path.join(__dirname, "defaults/scene-aux-seed.json"), "utf8");
    else if (command.endsWith(" seed")) output = JSON.stringify(seed);
    else if (command.endsWith(" read")) output = "a".repeat(64) + "\n" + JSON.stringify(settings);
    else if (command.endsWith(" status")) output = "features=fas_tuning\nlast_error=0\n";
    else if (command.includes(" save ")) {
      const match = command.match(/printf %s '([A-Za-z0-9+/=]+)'/);
      if (!match) { code = 1; error = "Invalid save envelope"; }
      else {
        const packet = Buffer.from(match[1], "base64");
        packets.push(packet);
        assert.ok([0x46505332, 0x46505333].includes(packet.readUInt32LE(0)));
        const start = packet.readUInt32LE(0) === 0x46505333 ? 40 : 32;
        settings = JSON.parse(packet.subarray(start, start + packet.readUInt32LE(4)).toString("utf8"));
      }
    } else { code = 1; error = "Unexpected host test command"; }
    queueMicrotask(() => context.window[callback](code, output, error));
  }}};
  context = vm.createContext({window, document: {
    getElementById(id) { return elements[id]; },
    querySelectorAll() { return [elements.save, elements.refresh, elements.rollback]; },
  }, TextEncoder, TextDecoder, atob: value => Buffer.from(value, "base64").toString("binary"),
  btoa: value => Buffer.from(value, "binary").toString("base64")});
  for (const name of ["scene_protocol.js", "scene_aux_protocol.js", "protocol.js"])
    vm.runInContext(fs.readFileSync(path.join(__dirname, "webroot", name), "utf8"), context);
  await vm.runInContext(fs.readFileSync(path.join(__dirname, "webroot/app.js"), "utf8"), context);
  return {elements, calls, packets, settings: () => settings};
}

test("legacy settings and FAS selector remain independent from Scene", async () => {
  const state = await app(), e = state.elements;
  assert.equal(e.fasMode.value, "balance");
  assert.equal(e.fasMarginBase.value, "");
  assert.equal(e.fallbackFps.value, "");
  const advanced = JSON.parse(e.editor.value);
  advanced.sceneBaseMode = "balance";
  e.editor.value = JSON.stringify(advanced);
  e.editor.listeners.input();
  e.fasMode.value = "fast";
  e.fasMode.listeners.change();
  assert.equal(JSON.parse(e.editor.value).sceneMode, "performance");
  await e.save.onclick();
  assert.equal(state.settings().fasMode, "fast");
  assert.equal(state.settings().sceneMode, "performance");
  assert.equal(state.settings().sceneBaseMode, "balance");
  const packet = state.packets.at(-1), fas = packet.subarray(-80);
  assert.equal(fas.readUInt32LE(4), 3);
  assert.equal(fas.readUInt32LE(8), 0);
});

test("aux GUI edits preserve existing advanced configuration and do not turn it on for legacy reads", async () => {
  const state = await app(), e = state.elements;
  assert.equal(e.sceneAux.checked, false);
  const original = {...JSON.parse(e.editor.value), gpu: true, sceneMode: "powersave", sceneBaseMode: "balance",
    fallbackFps: [30,45,60,90,120,130,144,165], sceneApps: {"org.example.camera": {category: "Camera"}}};
  e.editor.value = JSON.stringify(original); e.editor.listeners.input();
  e.sceneAux.checked = true; e.sceneAux.listeners.change();
  await e.save.onclick();
  assert.equal(state.packets.at(-1).readUInt32LE(0), 0x46505333);
  assert.equal(state.settings().sceneAux, true);
  assert.equal(state.settings().gpu, true);
  assert.equal(state.settings().sceneMode, "powersave");
  assert.equal(state.settings().sceneBaseMode, "balance");
  assert.deepEqual(Array.from(state.settings().fallbackFps), original.fallbackFps);
  assert.deepEqual(JSON.parse(JSON.stringify(state.settings().sceneApps)), original.sceneApps);
  e.fasMode.value = "fast"; e.fasMode.listeners.change();
  await e.save.onclick();
  assert.equal(state.settings().sceneAux, true);
  assert.equal(state.settings().fasMode, "fast");
  assert.equal(state.settings().sceneBaseMode, "balance");
});

test("GPU auxiliary toggle is independent, preserved, and never materialized by unrelated legacy edits", async () => {
  const state=await app(),e=state.elements;
  assert.equal(e.sceneGpuLock.checked,true);assert.equal(e.sceneGpuLock.disabled,true);
  e.fasMode.value="fast";e.fasMode.listeners.change();await e.save.onclick();
  assert.equal(Object.hasOwn(state.settings(),"sceneGpuLock"),false);
  assert.equal(state.packets.at(-1).readUInt32LE(0),0x46505332);
  e.sceneAux.checked=true;e.sceneAux.listeners.change();
  assert.equal(e.sceneGpuLock.disabled,false);
  e.sceneGpuLock.checked=false;e.sceneGpuLock.listeners.change();await e.save.onclick();
  assert.equal(state.settings().sceneAux,true);assert.equal(state.settings().sceneGpuLock,false);
  const packet=state.packets.at(-1);assert.equal(packet.readUInt32LE(packet.length-26584+212),0);
  e.fasMode.value="balance";e.fasMode.listeners.change();await e.save.onclick();
  assert.equal(state.settings().sceneGpuLock,false);assert.equal(e.sceneGpuLock.checked,false);
  e.sceneAux.checked=false;e.sceneAux.listeners.change();await e.save.onclick();
  assert.equal(state.settings().sceneGpuLock,false);assert.equal(e.sceneGpuLock.disabled,true);
  assert.equal(state.packets.at(-1).readUInt32LE(0),0x46505332);
});

test("base and per-FPS controls reach the saved binary", async () => {
  const state = await app(), e = state.elements;
  e.fasMarginBase.value = "1.001";
  e.fasOverrides.value = '{"165":0.3}';
  e.fasMarginBase.listeners.change();
  await e.save.onclick();
  assert.deepEqual(state.settings().fasMarginFps, {base: 1.001, "165": 0.3});
  const fas = state.packets.at(-1).subarray(-80);
  assert.equal(fas.readUInt32LE(8), 1001);
  assert.equal(fas.readUInt32LE(12), 1);
  assert.equal(fas.readUInt32LE(16), 165);
  assert.equal(fas.readUInt32LE(20), 300);
});

test("invalid form edits cannot silently save an older valid margin", async () => {
  const state = await app(), e = state.elements;
  e.fasOverrides.value = '{"060":1}';
  e.fasOverrides.listeners.change();
  await e.save.onclick();
  assert.equal(state.packets.length, 0);
  assert.equal(e.message.dataset.error, "true");
  const corrected = JSON.parse(e.editor.value);
  corrected.fasMode = "powersave";
  e.editor.value = JSON.stringify(corrected);
  e.editor.listeners.input();
  await e.save.onclick();
  assert.equal(state.packets.length, 1);
  assert.equal(state.settings().fasMode, "powersave");
  assert.equal(state.settings().sceneMode, "performance");
});

test("browser-invalid numeric input cannot be treated as an empty default", async () => {
  const state = await app(), e = state.elements;
  e.fasMarginBase.value = "";
  e.fasMarginBase.validity = {badInput: true};
  e.fasMarginBase.listeners.change();
  await e.save.onclick();
  assert.equal(state.packets.length, 0);
  assert.equal(e.message.dataset.error, "true");
});

test("fallback FPS stays independent and blank restores legacy inheritance", async () => {
  const state = await app(), e = state.elements;
  const settings = JSON.parse(e.editor.value);
  settings.fps = [30, 60, 90, 120, 144, 165];
  settings.sceneMode = "powersave";
  settings.sceneBaseMode = "balance";
  e.editor.value = JSON.stringify(settings);
  e.editor.listeners.input();
  e.fallbackFps.value = "30, 45, 60, 90, 120, 130, 144, 165";
  e.fallbackFps.listeners.change();
  await e.save.onclick();
  assert.deepEqual(state.settings().fps, settings.fps);
  assert.deepEqual(state.settings().fallbackFps, [30, 45, 60, 90, 120, 130, 144, 165]);
  assert.equal(state.settings().sceneMode, "powersave");
  assert.equal(state.settings().sceneBaseMode, "balance");
  let packet = state.packets.at(-1), start = 32 + packet.readUInt32LE(4);
  assert.equal(packet.readUInt32LE(start + 140), 6);
  assert.equal(packet.readUInt32LE(start + 592 + 32 + 192), 8);
  e.fallbackFps.value = "";
  e.fallbackFps.listeners.change();
  await e.save.onclick();
  assert.equal(state.settings().fallbackFps, undefined);
  assert.equal(state.settings().sceneBaseMode, "balance", "Changing normal controls must preserve advanced import data");
  packet = state.packets.at(-1); start = 32 + packet.readUInt32LE(4);
  assert.equal(packet.readUInt32LE(start + 592 + 32 + 192), 6);
});

test("invalid fallback FPS cannot silently save previous settings", async () => {
  const state = await app(), e = state.elements;
  e.fallbackFps.value = "60, 60";
  e.fallbackFps.listeners.change();
  await e.save.onclick();
  assert.equal(state.packets.length, 0);
  assert.equal(e.message.dataset.error, "true");
});
