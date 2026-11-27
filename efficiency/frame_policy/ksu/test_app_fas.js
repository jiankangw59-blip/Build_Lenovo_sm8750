"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {test} = require("node:test");

async function app() {
  const elements = Object.create(null), calls = [], packets = [];
  const html = fs.readFileSync(path.join(__dirname, "webroot/index.html"), "utf8");
  const controlIds = Array.from(html.matchAll(/<(?:button|input|select|textarea)\b[^>]*\bid="([^"]+)"/g), match => match[1]);
  for (const [, id] of html.matchAll(/\bid="([^"]+)"/g))
    elements[id] = {value: "", textContent: "", placeholder: "", dataset: {}, listeners: {},
      addEventListener(name, callback) { this.listeners[name] = callback; }};
  elements.fasPreviewFps.value = "165";
  const seed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/seed.json"), "utf8"));
  const sceneSeed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/scene-seed.json"), "utf8"));
  let settings = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/settings.json"), "utf8"));
  settings.sceneMode = "performance";
  delete settings.fasMode; // Existing installed settings must remain readable.
  let status = "features=fas_tuning\nlast_error=0\n";
  let context;
  const window = {ksu: {exec(command, _options, callback) {
    calls.push(command);
    let output = "", error = "", code = 0;
    if (command.endsWith(" scene-seed")) output = JSON.stringify(sceneSeed);
    else if (command.endsWith(" aux-seed")) output = fs.readFileSync(path.join(__dirname, "defaults/scene-aux-seed.json"), "utf8");
    else if (command.endsWith(" seed")) output = JSON.stringify(seed);
    else if (command.endsWith(" read")) output = "a".repeat(64) + "\n" + JSON.stringify(settings);
    else if (command.endsWith(" status")) output = status;
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
    querySelectorAll(selector) {
      assert.equal(selector, "button, input, select, textarea");
      return controlIds.map(id => elements[id]);
    },
  }, TextEncoder, TextDecoder, atob: value => Buffer.from(value, "base64").toString("binary"),
  btoa: value => Buffer.from(value, "binary").toString("base64")});
  for (const name of ["scene_protocol.js", "scene_aux_protocol.js", "protocol.js"])
    vm.runInContext(fs.readFileSync(path.join(__dirname, "webroot", name), "utf8"), context);
  await vm.runInContext(fs.readFileSync(path.join(__dirname, "webroot/app.js"), "utf8"), context);
  return {elements, calls, packets, settings: () => settings, setStatus: value => { status = value; }};
}

test("the running kernel controls the visible FAS strength guidance", async () => {
  const state = await app(), e = state.elements;
  state.setStatus("configured=1\nowner=scene\nfas_mode=1\nfas_margin_base_mfps=1000\nfeatures=fas_tuning,fas_mode_strength_v1_other\n");
  await e.refresh.onclick();
  assert.equal(e.fasStrength.hidden, false);
  assert.match(e.fasStrengthNote.textContent, /帧率 ÷ 60.*最多再下调 3 FPS/);
  assert.equal(e["fas-performance-margin"].textContent, "0.825");
  assert.equal(e["fas-performance-target"].textContent, "161.175–164.175");
  e.fasMode.value = "performance";
  e.fasMode.listeners.change();
  assert.match(e.fasLiveStatus.textContent, /内核已载入均衡档.*由 Scene 调度/);
  assert.doesNotMatch(e.fasLiveStatus.textContent, /由游戏 FAS 调度/);
  const raw = "configured=1\r\r\nfeatures=fas_tuning,fas_mode_strength_v1\r\r\nfas_mode=2\r\r\nfas_util_limit_mfps=200\r\r\nowner=frame\r\r\nprobe=1\r\r\nobserve=0\r\r\nfas_effective_mfps=164500\r\r\n";
  state.setStatus(raw);
  await e.refresh.onclick();
  assert.equal(e.status.textContent, raw);
  assert.match(e.fasLiveStatus.textContent, /内核已载入性能档.*最多再下调 0\.2 FPS.*由游戏 FAS 调度.*164\.5 FPS/);
  assert.equal(e["fas-performance-target"].textContent, "164.5–164.7");
  state.setStatus(raw.replace("observe=0", "observe=1"));
  await e.refresh.onclick();
  assert.match(e.fasLiveStatus.textContent, /当前仅观察/);
  assert.doesNotMatch(e.fasLiveStatus.textContent, /由游戏 FAS 调度/);
  state.setStatus(raw.replace("probe=1", "probe=0"));
  await e.refresh.onclick();
  assert.doesNotMatch(e.fasLiveStatus.textContent, /由游戏 FAS 调度/);
  const original = e.editor.value;
  e.fasPreviewFps.value = "30";
  e.fasPreviewFps.listeners.input();
  assert.equal(e["fas-performance-margin"].textContent, "0.15");
  assert.equal(e.editor.value, original);
  assert.equal(state.packets.length, 0);
});

test("per-mode adaptive limits preserve zero and other modes, preview and reset", async () => {
  const state = await app(), e = state.elements;
  assert.equal(e.fasUtilLimit.disabled, true);
  state.setStatus("configured=1\nowner=scene\nfeatures=fas_tuning,fas_util_limit_v1\nfas_mode=1\nfas_util_limit_mfps=500\n");
  await e.refresh.onclick();
  assert.equal(e.fasUtilLimit.disabled, false);
  e.fasUtilLimit.value = "0.501"; e.fasUtilLimit.listeners.change();
  e.fasMode.value = "performance"; e.fasMode.listeners.change();
  assert.equal(e.fasUtilLimit.value, "");
  e.fasUtilLimit.value = "0"; e.fasUtilLimit.listeners.change();
  assert.equal(e["fas-performance-extra"].textContent, "0");
  assert.equal(e["fas-performance-target"].textContent, "164.175");
  e.fasMode.value = "balance"; e.fasMode.listeners.change();
  assert.equal(e.fasUtilLimit.value, "0.501");
  assert.match(e.fasLiveStatus.textContent, /最多再下调 0\.5 FPS/);
  await e.save.onclick();
  assert.deepEqual(state.settings().fasUtilLimits, {powersave:null,balance:0.501,performance:0,fast:null});
  let fas = state.packets.at(-1).subarray(-88);
  assert.equal(fas.readUInt32LE(80), 501); assert.equal(fas.readUInt32LE(84), 0);
  e.fasReset.onclick(); await e.save.onclick();
  assert.equal(state.settings().fasUtilLimits.balance, null);
  assert.equal(state.settings().fasUtilLimits.performance, 0);
  assert.equal(state.packets.at(-1).readUInt32LE(20), 80);
});

test("unsupported kernels and invalid adaptive limits cannot silently save", async () => {
  const state = await app(), e = state.elements;
  assert.match(e.fasUtilLimitNote.textContent, /待保存预览中的自定义上限需更新内核才能应用/);
  for (const limit of [0, -1, 3.001, 0.0001]) {
    const settings = JSON.parse(e.editor.value);
    settings.fasUtilLimits = {balance:limit};
    e.editor.value = JSON.stringify(settings); e.editor.listeners.input();
    await e.save.onclick();
    assert.equal(state.packets.length, 0);
    assert.equal(e.message.dataset.error, "true");
  }
});

test("reset discards browser-invalid adaptive limit input", async () => {
  const state = await app(), e = state.elements;
  state.setStatus("features=fas_tuning,fas_util_limit_v1\n");
  await e.refresh.onclick();
  e.fasUtilLimit.value = "0.501";
  e.fasUtilLimit.listeners.change();
  e.fasUtilLimit.value = "";
  e.fasUtilLimit.validity = {badInput: true};
  e.fasUtilLimit.listeners.change();
  assert.equal(e.message.dataset.error, "true");
  e.fasReset.onclick();
  assert.equal(e.message.dataset.error, "false");
  assert.equal(JSON.parse(e.editor.value).fasUtilLimits.balance, null);
  assert.equal(e["fas-balance-extra"].textContent, "3");
});

test("round trips lock all editors until the running state returns", async () => {
  const state = await app(), e = state.elements;
  assert.equal(e.sceneGpuLock.disabled, true);
  e.sceneAux.checked = true;
  e.sceneAux.listeners.change();
  assert.equal(e.sceneGpuLock.disabled, false);
  const pending = e.save.onclick();
  const controls = [e.fasMode, e.fasMarginBase, e.fasOverrides, e.fasPreviewFps,
    e.fasReset, e.fallbackFps, e.sceneAux, e.sceneGpuLock, e.editor, e.save, e.refresh, e.rollback];
  for (const control of controls) assert.equal(control.disabled, true);
  await pending;
  for (const control of controls) assert.equal(control.disabled, false);

  e.sceneAux.checked = false;
  e.sceneAux.listeners.change();
  await e.save.onclick();
  assert.equal(e.sceneAux.disabled, false);
  assert.equal(e.sceneGpuLock.disabled, true);
});

test("legacy settings and FAS selector remain independent from Scene", async () => {
  const state = await app(), e = state.elements;
  assert.equal(e.fasMode.value, "balance");
  assert.equal(e.fasMarginBase.value, "");
  assert.equal(e.fallbackFps.value, "");
  const advanced = JSON.parse(e.editor.value);
  advanced.fasMarginFps = 1;
  advanced.sceneBaseMode = "balance";
  advanced.games = {"org.example.game": [60, 165]};
  advanced.sceneApps = {"org.example.scene": "powersave"};
  e.editor.value = JSON.stringify(advanced);
  e.editor.listeners.input();
  assert.equal(e.fasMarginBase.value, "1");
  e.fasMode.value = "fast";
  e.fasMode.listeners.change();
  assert.equal(e.fasMarginBase.value, "");
  assert.equal(JSON.parse(e.editor.value).sceneMode, "performance");
  await e.save.onclick();
  assert.equal(state.settings().fasMode, "fast");
  assert.equal(state.settings().fasMarginFps, undefined);
  assert.deepEqual(state.settings().fasProfiles, {powersave: null, balance: 1, performance: null, fast: null});
  assert.deepEqual(state.settings().games, advanced.games);
  assert.deepEqual(state.settings().sceneApps, advanced.sceneApps);
  assert.deepEqual(state.settings().gameCpuMHz, advanced.gameCpuMHz);
  assert.equal(state.settings().sceneMode, "performance");
  assert.equal(state.settings().sceneBaseMode, "balance");
  const packet = state.packets.at(-1), fas = packet.subarray(-80);
  assert.equal(fas.readUInt32LE(4), 3);
  assert.equal(fas.readUInt32LE(8), 0);
});

test("aux GUI edits preserve existing advanced configuration and do not turn it on for legacy reads", async () => {
  const state = await app(), e = state.elements;
  assert.equal(e.sceneAux.checked, false);
  const original = {...JSON.parse(e.editor.value), fasMarginFps: 1, gpu: true, sceneMode: "powersave", sceneBaseMode: "balance",
    fallbackFps: [30,45,60,90,120,130,144,165], sceneApps: {"org.example.camera": {category: "Camera"}}};
  e.editor.value = JSON.stringify(original); e.editor.listeners.input();
  e.sceneAux.checked = true; e.sceneAux.listeners.change();
  await e.save.onclick();
  assert.equal(state.packets.at(-1).readUInt32LE(0), 0x46505333);
  assert.equal(state.settings().sceneAux, true);
  assert.equal(state.settings().fasMarginFps, 1);
  assert.equal(state.settings().fasProfiles, undefined);
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
  e.fasMode.value = "performance";
  e.fasMode.listeners.change();
  assert.equal(e.fasMarginBase.value, "");
  e.fasMarginBase.value = "0.45";
  e.fasMarginBase.listeners.change();
  e.fasMode.value = "balance";
  e.fasMode.listeners.change();
  assert.equal(e.fasMarginBase.value, "1.001");
  assert.deepEqual(JSON.parse(e.fasOverrides.value), {"165": 0.3});
  assert.equal(e["fas-balance-margin"].textContent, "0.3（覆盖）");
  await e.save.onclick();
  assert.deepEqual(state.settings().fasProfiles.balance, {base: 1.001, "165": 0.3});
  assert.equal(state.settings().fasProfiles.performance, 0.45);
  const fas = state.packets.at(-1).subarray(-80);
  assert.equal(fas.readUInt32LE(8), 1001);
  assert.equal(fas.readUInt32LE(12), 1);
  assert.equal(fas.readUInt32LE(16), 165);
  assert.equal(fas.readUInt32LE(20), 300);
  e.fasReset.onclick();
  await e.save.onclick();
  assert.equal(state.settings().fasProfiles.balance, null);
  assert.equal(state.settings().fasProfiles.performance, 0.45);
});

test("invalid form edits cannot silently save an older valid margin", async () => {
  const state = await app(), e = state.elements;
  e.fasOverrides.value = '{"060":1}';
  e.fasMode.value = "fast";
  e.fasMode.listeners.change();
  assert.equal(e.fasMode.value, "balance");
  assert.equal(e.fasOverrides.value, '{"060":1}');
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
