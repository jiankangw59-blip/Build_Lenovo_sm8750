"use strict";

function frameSettingsProtocol(settings, seed) {
  const fields = ["enabled", "cpu", "gpu", "observe", "sceneMode", "fps", "games", "relations", "gameCpuMHz"];
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
  if (!settings.games || typeof settings.games !== "object" || Array.isArray(settings.games))
    throw new Error("games 必须为包名到帧率列表的映射");
  const games = Object.create(null);
  for (const [name, values] of Object.entries(settings.games)) {
    if (!/^[A-Za-z0-9_.]{1,127}$/.test(name)) throw new Error(`包名不合法：${name}`);
    games[name] = rates(values);
  }
  const fromBase64 = value => Uint8Array.from(typeof atob === "function" ? atob(value) :
    Buffer.from(value, "base64").toString("binary"), c => c.charCodeAt(0));
  if (!Object.hasOwn(seed.profiles, settings.sceneMode)) throw new Error("场景模式不存在");
  const config = fromBase64(seed.profiles[settings.sceneMode]);
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
    const values = games[name] ?? fps;
    view.setUint32(start + 192, values.length, true);
    values.forEach((v, i) => view.setUint32(start + 196 + i * 4, v, true));
  }
  rule("", 0, seed.relations.fallback);
  names.forEach((name, i) => rule(name, i + 1, Object.hasOwn(seed.relations.rules, name) ?
    seed.relations.rules[name] : seed.relations.fallback));
  const json = new TextEncoder().encode(JSON.stringify(settings, null, 2) + "\n");
  if (json.length > 49152) throw new Error("配置文件过大");
  const payload = new Uint8Array(16 + json.length + config.length + table.length);
  const envelope = new DataView(payload.buffer);
  [json.length, config.length, table.length, settings.enabled ? 1 : 0].forEach((v, i) => envelope.setUint32(i * 4, v, true));
  payload.set(json, 16); payload.set(config, 16 + json.length); payload.set(table, 16 + json.length + config.length);
  return {payload, config, table};
}

if (typeof module !== "undefined") module.exports = {frameSettingsProtocol};
