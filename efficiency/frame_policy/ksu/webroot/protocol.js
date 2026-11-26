"use strict";

function frameFasProtocol(settings) {
  const modes = ["powersave", "balance", "performance", "fast"];
  const mode = Object.hasOwn(settings, "fasMode") ? settings.fasMode : "balance";
  const index = modes.indexOf(mode);
  if (index < 0) throw new Error("fasMode 必须为 powersave / balance / performance / fast");
  const milli = value => {
    if (typeof value !== "number" || !Number.isFinite(value) || value < 0)
      throw new Error("FAS 余量必须为非负有限数值");
    const text = String(value);
    if (!/^(0|[1-9]\d*)(\.\d{1,3})?$/.test(text))
      throw new Error("FAS 余量最多保留三位小数");
    const [whole, fraction = ""] = text.split(".");
    const result = Number(whole) * 1000 + Number(fraction.padEnd(3, "0"));
    if (!Number.isSafeInteger(result)) throw new Error("FAS 余量超出范围");
    return result;
  };
  const margin = Object.hasOwn(settings, "fasMarginFps") ? settings.fasMarginFps : [3, 1, 0.3, 0][index];
  let base, overrides = [];
  if (margin && typeof margin === "object" && !Array.isArray(margin)) {
    if (!Object.hasOwn(margin, "base")) throw new Error("高级 FAS 余量必须包含 base");
    base = milli(margin.base);
    for (const [key, value] of Object.entries(margin)) {
      if (key === "base") continue;
      if (!/^(0|[1-9]\d*)$/.test(key)) throw new Error("FAS 覆盖帧率必须为规范整数");
      const fps = Number(key), amount = milli(value);
      if (!Number.isSafeInteger(fps) || fps < 15 || fps > 240 || amount > fps * 1000 - 4000)
        throw new Error("FAS 帧率覆盖余量超出范围");
      overrides.push([fps, amount]);
    }
    overrides.sort((a, b) => a[0] - b[0]);
  } else base = milli(margin);
  if (base > 30000 || overrides.length > 8) throw new Error("FAS 基础余量或覆盖数量超出范围");
  const blob = new Uint8Array(80), view = new DataView(blob.buffer);
  [1, index, base, overrides.length].forEach((value, i) => view.setUint32(i * 4, value, true));
  overrides.forEach(([fps, amount], i) => {
    view.setUint32(16 + i * 8, fps, true);
    view.setUint32(20 + i * 8, amount, true);
  });
  return {blob, mode, base, overrides};
}

function frameSettingsProtocol(settings, seed, sceneSeed) {
  const fields = ["enabled", "cpu", "gpu", "observe", "sceneMode", "sceneBaseMode", "sceneApps", "fasMode", "fasMarginFps", "fps", "fallbackFps", "games", "relations", "gameCpuMHz"];
  for (const key of Object.keys(settings)) if (!fields.includes(key)) throw new Error(`未知配置项：${key}`);
  for (const key of ["enabled", "cpu", "gpu", "observe", "relations"])
    if (typeof settings[key] !== "boolean") throw new Error(`${key} 必须为布尔值`);
  if (!settings.cpu && !settings.gpu && settings.enabled) throw new Error("请至少启用 CPU 或 GPU 控制");
  const rates = value => {
    value = typeof value === "number" ? [value] : value;
    if (!Array.isArray(value) || !value.length || value.length > 8 || value.some((v, i) =>
      !Number.isInteger(v) || v < 15 || v > 240 || (i && v <= value[i - 1])))
      throw new Error("帧率应为 15～240 范围内、递增且不重复的 1～8 个整数");
    return value;
  };
  const fps = rates(settings.fps);
  const fallback = Object.hasOwn(settings, "fallbackFps") ? rates(settings.fallbackFps) : fps;
  if (!settings.games || typeof settings.games !== "object" || Array.isArray(settings.games))
    throw new Error("games 必须为包名到帧率列表的映射");
  const games = Object.create(null);
  for (const [name, values] of Object.entries(settings.games)) {
    if (!/^[A-Za-z0-9_.]{1,127}$/.test(name)) throw new Error(`包名不合法：${name}`);
    games[name] = rates(values);
  }
  const fromBase64 = value => Uint8Array.from(typeof atob === "function" ? atob(value) :
    Buffer.from(value, "base64").toString("binary"), c => c.charCodeAt(0));
  const selectedBase = Object.hasOwn(settings, "sceneBaseMode") ? settings.sceneBaseMode : settings.sceneMode;
  if (!["powersave", "balance", "performance", "fast"].includes(selectedBase))
    throw new Error("sceneBaseMode 必须为合法的 Scene 模式");
  const baseMode = selectedBase === "fast" ? "balance" : selectedBase;
  if (!Object.hasOwn(seed.profiles, baseMode)) throw new Error("场景模式不存在");
  const config = fromBase64(seed.profiles[baseMode]);
  if (config.length !== 592) throw new Error("配置模板版本不匹配");
  const cfg = new DataView(config.buffer, config.byteOffset, config.byteLength);
  cfg.setUint32(4, (settings.cpu ? 1 : 0) | (settings.gpu ? 2 : 0) | (settings.observe ? 4 : 0), true);
  cfg.setUint32(12, fps.at(-1), true);
  cfg.setUint32(140, fps.length, true);
  for (let i = 0; i < 8; i++) cfg.setUint32(560 + i * 4, fps[i] ?? 0, true);
  if (!Array.isArray(settings.gameCpuMHz) || settings.gameCpuMHz.length !== seed.cpuIds.length)
    throw new Error("游戏 CPU 频率范围数量错误");
  settings.gameCpuMHz.forEach((limits, i) => {
    if (!Array.isArray(limits) || limits.length !== 2 || limits.some(v => !Number.isFinite(v)))
      throw new Error("CPU 频率范围格式错误");
    const khz = limits.map(v => Math.round(v * 1000));
    if (khz[0] < seed.cpuBounds[i][0] || khz[1] > seed.cpuBounds[i][1] || khz[0] > khz[1])
      throw new Error(`policy${seed.cpuIds[i]} 的频率超出硬件范围`);
    cfg.setUint32(144 + i * 104 + 16, khz[0], true);
    cfg.setUint32(144 + i * 104 + 20, khz[1], true);
  });

  const names = [...new Set([...Object.keys(seed.relations.rules), ...Object.keys(games)])].sort();
  if (names.length > 32) throw new Error("逐游戏配置超过 32 项");
  const table = new Uint8Array(7556), view = new DataView(table.buffer);
  [1, names.length, seed.cpuIds.length, 0, ...seed.cpuIds].forEach((v, i) => view.setUint32(i * 4, v, true));
  function rule(name, index, overrides) {
    const start = 32 + index * 228;
    table.set(new TextEncoder().encode(name), start);
    const relations = settings.relations ? overrides : {};
    for (let i = 0; i < 4; i++) {
      const rel = relations[i] ?? [0, 0, 0, 0];
      view.setUint32(start + 128 + i * 16, rel[0], true);
      view.setUint32(start + 132 + i * 16, rel[1], true);
      view.setInt32(start + 136 + i * 16, rel[2], true);
      view.setInt32(start + 140 + i * 16, rel[3], true);
    }
    const values = games[name] ?? fallback;
    view.setUint32(start + 192, values.length, true);
    values.forEach((v, i) => view.setUint32(start + 196 + i * 4, v, true));
  }
  rule("", 0, seed.relations.fallback);
  names.forEach((name, i) => rule(name, i + 1, Object.hasOwn(seed.relations.rules, name) ?
    seed.relations.rules[name] : seed.relations.fallback));
  const tuning = frameFasProtocol(settings), fas = tuning.blob;
  const normalized = {...settings, fasMode: tuning.mode};
  const json = new TextEncoder().encode(JSON.stringify(normalized, null, 2) + "\n");
  if (json.length > 49152) throw new Error("配置文件过大");
  const encodeScene = typeof module !== "undefined" ? require("./scene_protocol.js").frameSceneProtocol : frameSceneProtocol;
  const scene = encodeScene(settings, sceneSeed).table;
  const requiresScene = settings.sceneMode === "fast" || Object.keys(settings.sceneApps ?? {}).length > 0 ||
    selectedBase !== settings.sceneMode;
  const payload = new Uint8Array(32 + json.length + config.length + table.length + scene.length + fas.length);
  const envelope = new DataView(payload.buffer);
  [0x46505332, json.length, config.length, table.length, scene.length, fas.length, settings.enabled ? 1 : 0,
    requiresScene ? 1 : 0].forEach((v, i) => envelope.setUint32(i * 4, v, true));
  payload.set(json, 32); payload.set(config, 32 + json.length);
  payload.set(table, 32 + json.length + config.length);
  payload.set(scene, 32 + json.length + config.length + table.length);
  payload.set(fas, payload.length - fas.length);
  return {payload, config, table, scene, fas, settings: normalized};
}

if (typeof module !== "undefined") module.exports = {frameSettingsProtocol, frameFasProtocol};
