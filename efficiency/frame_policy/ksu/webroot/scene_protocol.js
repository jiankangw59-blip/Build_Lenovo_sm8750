"use strict";

/* Numeric tables only: no scripts, polling, resident process or device writes. */
function frameSceneProtocol(settings, seed) {
  if (!settings || typeof settings !== "object" || Array.isArray(settings))
    throw new Error("Scene 配置必须为对象");
  if (!seed || seed.abi !== 1 || seed.tableSize !== 14384 || seed.profileSize !== 336)
    throw new Error("Scene 数值配置库版本不匹配");
  const ids = seed.cpuIds;
  if (!Array.isArray(ids) || !ids.length || ids.length > 4 ||
      ids.some(v => !Number.isInteger(v) || v < 0 || v > 4095) || new Set(ids).size !== ids.length)
    throw new Error("Scene CPU 策略拓扑无效");
  if (!Array.isArray(seed.cpuBounds) || seed.cpuBounds.length !== ids.length ||
      !Array.isArray(seed.profiles) || !seed.profiles.length)
    throw new Error("Scene 数值配置库缺少硬件范围或频率配置");
  const validMode = value => ["powersave", "balance", "performance", "fast"].includes(value);
  const mode = settings.sceneMode;
  if (!validMode(mode)) throw new Error("Scene 模式应为 powersave、balance、performance 或 fast");
  const apps = settings.sceneApps === undefined ? {} : settings.sceneApps;
  if (!apps || typeof apps !== "object" || Array.isArray(apps))
    throw new Error("sceneApps 必须为包名到模式/类别的映射");
  const names = Object.keys(apps).sort();
  if (names.length > 64) throw new Error("Scene 逐应用配置超过 64 项");
  const own = (object, key) => object !== null && typeof object === "object" && Object.hasOwn(object, key);
  function indices(appMode, category) {
    if (appMode === "ignore" || appMode === "igoned") return seed.ignored;
    if (!validMode(appMode) || !own(seed.selectors, appMode)) throw new Error("Scene 应用模式不存在");
    if (own(seed.categoryAliases, category)) category = seed.categoryAliases[category];
    if (!own(seed.selectors[appMode], category)) throw new Error(`Scene 应用类别不存在：${category}`);
    return seed.selectors[appMode][category];
  }
  const defaults = indices(mode, "normal");
  const rules = names.map(name => {
    if (!/^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+$/.test(name) || name.length > 127)
      throw new Error(`Scene 包名不合法：${name}`);
    const raw = apps[name], value = typeof raw === "string" ? {mode: raw} : raw;
    if (!value || typeof value !== "object" || Array.isArray(value) ||
        Object.keys(value).some(key => key !== "mode" && key !== "category"))
      throw new Error(`Scene 应用配置格式错误：${name}`);
    const category = value.category ?? "normal";
    if (typeof category !== "string") throw new Error("Scene 应用类别应为字符串");
    return {name, indices: indices(value.mode ?? mode, category)};
  });

  const selected = [], slots = new Map();
  function packIndices(values) {
    if (!Array.isArray(values) || values.length !== 3)
      throw new Error("Scene 配置必须包含 active/inactive/off 三个状态");
    return values.map(index => {
      if (!Number.isInteger(index) || index < 0 || index >= seed.profiles.length)
        throw new Error("Scene 数值配置索引越界");
      if (!slots.has(index)) {
        if (selected.length === 16) throw new Error("所选 Scene 组合超过内核 16 个配置的容量，请减少不同组合");
        slots.set(index, selected.length); selected.push(index);
      }
      return slots.get(index);
    });
  }
  const packedDefaults = packIndices(defaults);
  const packedRules = rules.map(rule => ({name: rule.name, profiles: packIndices(rule.indices)}));
  const table = new Uint8Array(14384), view = new DataView(table.buffer);
  const write = (offset, value) => view.setUint32(offset, value, true);
  [1, names.length, ids.length, selected.length, ...ids, ...Array(4 - ids.length).fill(0),
    ...packedDefaults, 0].forEach((value, index) => write(index * 4, value));
  const limits = (values, label) => {
    if (!Array.isArray(values) || values.length !== 2 || values.some(v => !Number.isInteger(v)) ||
        values[0] < 0 || values[0] > values[1] || values[1] <= 0 || values[1] > 10000000)
      throw new Error(`Scene ${label}频率范围无效`);
    return values;
  };
  selected.forEach((sourceIndex, packedIndex) => {
    const profile = seed.profiles[sourceIndex], start = 48 + packedIndex * 336;
    if (!profile || typeof profile.enabled !== "boolean") throw new Error("Scene 数值配置无效");
    if (!profile.enabled) {
      if (Object.keys(profile).some(key => key !== "enabled")) throw new Error("Scene 忽略配置含有额外请求");
      return;
    }
    if (!Number.isInteger(profile.gpuPercent) || profile.gpuPercent < 0 || profile.gpuPercent > 200)
      throw new Error("Scene GPU 比例无效");
    const gpu = limits(profile.gpuKHz, "GPU");
    if (!Array.isArray(profile.cpus) || profile.cpus.length !== ids.length)
      throw new Error("Scene 数值配置的 CPU 数量错误");
    [1, profile.gpuPercent, ...gpu].forEach((value, index) => write(start + index * 4, value));
    profile.cpus.forEach((cpu, i) => {
      const bounds = limits(cpu.kHz, "CPU"), hardware = limits(seed.cpuBounds[i], "硬件");
      if (bounds[0] < hardware[0] || bounds[1] > hardware[1]) throw new Error("Scene CPU 频率超出硬件范围");
      const steps = cpu.margins;
      if (!Array.isArray(steps) || steps.length > 8 || steps.some((step, index) =>
        !Array.isArray(step) || step.length !== 2 || !Number.isInteger(step[0]) ||
        !Number.isInteger(step[1]) || step[0] < 0 || step[0] > 0xffffffff ||
        step[1] < 1 || step[1] > 1000 || (!index && step[0] !== 0) ||
        (index && step[0] <= steps[index - 1][0]))) throw new Error("Scene CPU margin 阶梯无效");
      const offset = start + 16 + i * 80;
      [...bounds, steps.length, 0].forEach((value, index) => write(offset + index * 4, value));
      steps.forEach((step, index) => {
        write(offset + 16 + index * 8, step[0]); write(offset + 20 + index * 8, step[1]);
      });
    });
  });
  const encoder = new TextEncoder();
  packedRules.forEach((rule, index) => {
    const start = 5424 + index * 140;
    table.set(encoder.encode(rule.name), start);
    rule.profiles.forEach((value, i) => write(start + 128 + i * 4, value));
  });
  return {table, profileCount: selected.length, selectedLibraryProfiles: selected,
    defaults: packedDefaults, rules: packedRules};
}

if (typeof module !== "undefined") module.exports = {frameSceneProtocol};
