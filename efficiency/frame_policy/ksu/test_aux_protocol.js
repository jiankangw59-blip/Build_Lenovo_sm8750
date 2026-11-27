"use strict";
const {test} = require("node:test"), assert = require("node:assert/strict"), fs = require("node:fs");
const {frameSettingsProtocol} = require("./webroot/protocol.js");
const {frameSceneAuxProtocol} = require("./webroot/scene_aux_protocol.js");
const load = name => JSON.parse(fs.readFileSync(`${__dirname}/defaults/${name}.json`, "utf8"));
const seed = load("seed"), scene = load("scene-seed"), aux = load("scene-aux-seed");
const settings = () => load("settings");
const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
test("optional aux is off for legacy settings and preserves all existing numeric blobs", () => {
  const old = settings(), before = frameSettingsProtocol(old, seed, scene);
  const enabled = {...old, sceneAux: true};
  const after = frameSettingsProtocol(enabled, seed, scene, aux);
  for (const key of ["config", "table", "scene", "fas"]) assert.deepEqual(after[key], before[key]);
  assert.equal(view(before.payload).getUint32(0, true), 0x46505332);
  assert.equal(view(after.payload).getUint32(0, true), 0x46505333);
  assert.deepEqual(after.settings, {...before.settings, sceneAux: true});
  assert.equal(view(before.aux).getUint32(12, true), 0);
});
test("FPS3 carries exact ABI1 sizes, immutable runtime placeholders and signed boost", () => {
  const result = frameSettingsProtocol({...settings(), sceneMode: "powersave", sceneAux: true}, seed, scene, aux);
  const header = view(result.payload), data = view(result.aux);
  assert.deepEqual(Array.from({length: 8}, (_, i) => header.getUint32(8 + i * 4, true)), [592, 7556, 14384, 80, 26584, 0, 0, 1]);
  assert.equal(result.aux.length, 26584); assert.equal(data.getBigUint64(48, true), 0n);
  for (let i = 0; i < 28; i++) assert.equal(data.getInt32(56 + i * 4, true), -1);
  const active = data.getUint32(32, true), inactive = data.getUint32(36, true);
  assert.equal(data.getInt32(216 + active * 544 + 16, true), -2);
  assert.equal(data.getInt32(216 + inactive * 544 + 16, true), -5);
  assert.equal(data.getUint32(216 + active * 544 + 44, true), 2);
  assert.equal(data.getUint32(216 + active * 544 + 48, true), 0x100);
});
test("aux selectors do not collapse distinct original states that share frequency profiles", () => {
  const config = {...settings(), sceneAux: true, sceneApps: {"org.example.balance": "balance", "org.example.performance": "performance"}};
  const p = frameSettingsProtocol(config, seed, scene, aux), a = view(p.aux), s = view(p.scene);
  assert.equal(s.getUint32(5424 + 132, true), s.getUint32(5424 + 140 + 132, true));
  assert.notEqual(a.getUint32(17624 + 132, true), a.getUint32(17624 + 140 + 132, true));
});
test("all original mode/category selectors fit and all six legacy fields remain unchanged", () => {
  for (const mode of ["powersave", "balance", "performance", "fast"])
    for (const category of ["normal", "Camera", "ELP"]) {
      const result = frameSceneAuxProtocol({sceneMode: mode, sceneAux: true, sceneApps: {"org.example.application": {mode, category}}}, aux);
      assert.ok(result.profileCount <= 32); assert.equal(result.rules.length, 1);
    }
  const s = {...settings(), gpu: true, sceneMode: "powersave", sceneBaseMode: "balance", fasMode: "fast", fallbackFps: [30,45,60,90,120,130,144,165]};
  const before = frameSettingsProtocol(s, seed, scene), after = frameSettingsProtocol({...s, sceneAux: true}, seed, scene, aux);
  for (const key of ["config", "table", "scene", "fas"]) assert.deepEqual(before[key], after[key]);
  assert.equal(after.settings.sceneBaseMode, "balance"); assert.equal(after.settings.gpu, true);
});
test("typed aux rejects malformed controls, metadata and missing capabilities without wrapping", () => {
  assert.throws(() => frameSceneAuxProtocol({sceneAux: "true"}, aux));
  assert.throws(() => frameSceneAuxProtocol({...settings(), sceneAux: true}, null));
  for (const edit of [s => {s.profiles[0].cpus[0].waltBoost = -101;}, s => {s.profiles[0].cpus[0].validMask = 32;},
    s => {s.profiles[0].cpus[0].limiterClustersPacked = 0;}, s => {s.profiles[0].cpus[1].limiterClusterCount = 1;},
    s => {s.profiles[0].cpus[0].prefer = 4;}, s => {s.profiles[0].cpus[0].excludesMask = "0x10000000000000000";}]) {
    const bad = structuredClone(aux); edit(bad);
    assert.throws(() => frameSceneAuxProtocol({...settings(), sceneMode: "powersave", sceneAux: true}, bad));
  }
});

test("full CPU records carry original additive kHz and global known-default provenance", () => {
  const p = frameSettingsProtocol({...settings(), sceneMode: "powersave", sceneAux: true}, seed, scene, aux);
  const a = view(p.aux), at = 216 + a.getUint32(32, true) * 544;
  assert.deepEqual(Array.from({length: 12}, (_, i) => a.getUint32(168 + i * 4, true)),
    [2047, 1, 1, 1, 0, 0, 1, 0, 0, 1, 185, 1]);
  assert.equal(aux.optionOrigins.statSource, "native-default-key-missing");
  assert.equal(aux.optionOrigins.launcherBoost, "explicit-native-pref");
  assert.equal(aux.optionOrigins.gpuLock, "explicit-native-pref");
  assert.equal(a.getUint32(at + 68, true), 3);
  assert.deepEqual(Array.from({length: 3}, (_, i) => [a.getUint32(at + 72 + i * 8, true), a.getInt32(at + 76 + i * 8, true)]),
    [[0,150000],[1152000,100000],[1785600,70000]]);
  assert.deepEqual(Array.from({length: 3}, (_, i) => a.getUint32(at + 56 + i * 4, true)), [0,0,0]);
  assert.equal(a.getUint32(at + 52, true), 127);
});

test("Camera never inherits the normal p3 feedback; fast and off remain empty feedback", () => {
  const config = {...settings(), sceneMode: "performance", sceneAux: true,
    sceneApps: {"org.example.camera": {mode: "performance", category: "Camera"}}};
  const a = view(frameSettingsProtocol(config, seed, scene, aux).aux);
  const normal = a.getUint32(32, true), camera = a.getUint32(17624 + 128, true);
  assert.notEqual(normal, camera);
  assert.equal(a.getUint32(216 + normal * 544 + 68, true), 3);
  for (let cpu = 0; cpu < 4; cpu++) {
    const base = 216 + camera * 544 + cpu * 136;
    assert.equal(a.getUint32(base + 52, true), 0); assert.equal(a.getUint32(base + 68, true), 0);
  }
  for (const mode of ["powersave", "balance", "performance", "fast"])
    for (const category of ["normal", "ELP", "Camera"]) {
      const [active, inactive, off] = aux.selectors[mode][category];
      assert.ok(aux.profiles[off].cpus.every(cpu => Object.entries(cpu).every(([key,value]) =>
        key === "additive" ? value.length === 0 : key === "excludesMask" ? value === "0x0" : value === 0)));
      if (mode === "fast" || category === "Camera")
        for (const index of [active, inactive]) assert.ok(aux.profiles[index].cpus.every(cpu => cpu.feedbackMask === 0));
    }
});

test("new feedback/options reject invalid units, unused fields and additive ordering", () => {
  const edits = [s => {s.options.source=3;}, s => {s.options.panelPeakHz=0;}, s => {s.options.validMask=2048;},
    s => {s.options.statSource=2;}, s => {s.options.gestureDurationMs=60001;}, s => {s.options.privatePackages=[];},
    s => {s.profiles[0].cpus[0].perfectKHz=10000001;}, s => {s.profiles[0].cpus[0].smoothness=33;},
    s => {s.profiles[0].cpus[0].mtMilliPercent=100001;}, s => {s.profiles[0].cpus[0].additive[0][0]=1;},
    s => {s.profiles[0].cpus[0].additive[1][0]=0;}, s => {s.profiles[0].cpus[0].additive[0][1]=-10000001;},
    s => {s.profiles[0].cpus[0].feedbackMask&=~64;}, s => {s.profiles[0].cpus[0].feedbackMask=128;},
    s => {s.options.gpuLock=2;}, s => {s.options.validMask&=~1024;}];
  for (const edit of edits) {
    const bad=structuredClone(aux);edit(bad);
    assert.throws(() => frameSceneAuxProtocol({...settings(),sceneMode:"powersave",sceneAux:true},bad));
  }
  const signed=structuredClone(aux);signed.profiles[0].cpus[0].additive[0][1]=-150000;
  const output=frameSceneAuxProtocol({...settings(),sceneMode:"powersave",sceneAux:true},signed);
  assert.equal(view(output.table).getInt32(216+76,true),-150000);
});

test("independent GPU auxiliary override changes only its typed word and is inert with aux off", () => {
  const legacy = settings(), untouched = frameSettingsProtocol(legacy, seed, scene, aux);
  assert.equal(view(untouched.payload).getUint32(0,true),0x46505332);
  assert.equal(Object.hasOwn(untouched.settings,"sceneGpuLock"),false);
  const enabled = frameSettingsProtocol({...legacy,sceneAux:true},seed,scene,aux);
  const disabled = frameSettingsProtocol({...legacy,sceneAux:true,sceneGpuLock:false},seed,scene,aux);
  assert.equal(view(enabled.aux).getUint32(212,true),1);
  assert.equal(view(disabled.aux).getUint32(212,true),0);
  for(const key of ["config","table","scene","fas"])assert.deepEqual(enabled[key],disabled[key]);
  assert.deepEqual(enabled.aux.subarray(0,212),disabled.aux.subarray(0,212));
  assert.deepEqual(enabled.aux.subarray(216),disabled.aux.subarray(216));
  assert.equal(disabled.settings.sceneAux,true);assert.equal(disabled.settings.sceneGpuLock,false);
  const inactive=frameSettingsProtocol({...legacy,sceneGpuLock:false},seed,scene,aux);
  assert.equal(view(inactive.payload).getUint32(0,true),0x46505332);
  assert.deepEqual(inactive.aux,untouched.aux);
  assert.throws(()=>frameSettingsProtocol({...legacy,sceneGpuLock:"false"},seed,scene,aux));
  assert.equal(aux.options.gpuLock,1); // An override never mutates the shared imported library.
});
