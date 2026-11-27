"use strict";
const backend = "sh /data/adb/modules/frame_policy_config/config.sh";
let counter = 0, savedHash = "", seed, sceneSeed, auxSeed, busy = false;
const editor = document.getElementById("editor"), message = document.getElementById("message");
const fasMode = document.getElementById("fasMode"), fasBase = document.getElementById("fasMarginBase");
const fasOverrides = document.getElementById("fasOverrides");
const fasUtilLimit = document.getElementById("fasUtilLimit");
const fasPreviewFps = document.getElementById("fasPreviewFps");
const fallbackFps = document.getElementById("fallbackFps");
const sceneAux = document.getElementById("sceneAux");
const sceneGpuLock = document.getElementById("sceneGpuLock");
const formControls = document.querySelectorAll("button, input, select, textarea");
const fasDefaults = {powersave: 3, balance: 1, performance: 0.3, fast: 0};
const fasLabels = {powersave: "省电", balance: "均衡", performance: "性能", fast: "快速"};
const adaptiveLimits = {powersave: 3000, balance: 500, performance: 200, fast: 0};
let fasEditError = "", otherEditError = "", editingMode = "balance", displayedBase = "", displayedOverrides = "";
let displayedUtilLimit = "", supportsUtilLimit = false;
let supportsFas = false, supportsStrength = false;
let gpuLockEdited = false;
function showFasProfile(settings) {
  editingMode = frameFasProtocol(settings).mode;
  fasMode.value = editingMode;
  fasBase.placeholder = String(fasDefaults[editingMode]);
  const margin = frameFasProfiles(settings)[editingMode];
  const advanced = margin !== null && typeof margin === "object";
  displayedBase = fasBase.value = margin === null ? "" : String(advanced ? margin.base : margin);
  displayedOverrides = fasOverrides.value = JSON.stringify(advanced
    ? Object.fromEntries(Object.entries(margin).filter(([key]) => key !== "base")) : {}, null, 2);
  const limit = frameFasUtilLimits(settings)[editingMode];
  displayedUtilLimit = fasUtilLimit.value = limit === null ? "" : String(limit);
  fasUtilLimit.disabled = busy || !supportsUtilLimit;
}
function showSettings(settings) {
  showFasProfile(settings);
  sceneAux.checked = settings.sceneAux ?? false;
  sceneGpuLock.checked = settings.sceneGpuLock ?? Boolean(auxSeed?.options?.gpuLock);
  sceneGpuLock.disabled = busy || !sceneAux.checked;
  gpuLockEdited = false;
  fallbackFps.value = settings.fallbackFps === undefined ? "" :
    (Array.isArray(settings.fallbackFps) ? settings.fallbackFps.join(", ") : String(settings.fallbackFps));
}
function showFasPreview(settings) {
  const fps = Number(fasPreviewFps.value);
  const validFps = Number.isInteger(fps) && fps >= 15 && fps <= 240;
  document.getElementById("fasPreviewMessage").textContent = validFps ? "" : "请输入 15～240 的整数帧率。";
  document.getElementById("fasStrength").hidden = !supportsFas || !validFps || Boolean(fasEditError);
  if (!supportsFas || !validFps || fasEditError) return;
  const profiles = frameFasProfiles(settings);
  const utilLimits = frameFasUtilLimits(settings);
  for (const mode of Object.keys(fasLabels)) {
    const tuning = frameFasProtocol({fasMode: mode, fasProfiles: profiles, fasUtilLimits: utilLimits});
    const override = tuning.overrides.find(([rate]) => rate === fps);
    const margin = override?.[1] ?? (supportsStrength && fps >= 60 ? tuning.base : Math.floor(tuning.base * fps / 60));
    const extra = tuning.utilLimit ?? (supportsStrength ? adaptiveLimits[mode] : 3000);
    const lower = Math.max(1000, fps * 1000 - margin - extra) / 1000;
    const upper = Math.max(1000, fps * 1000 - margin) / 1000;
    document.getElementById(`fas-${mode}-margin`).textContent = `${margin / 1000}${override ? "（覆盖）" : ""}`;
    document.getElementById(`fas-${mode}-extra`).textContent = String(extra / 1000);
    document.getElementById(`fas-${mode}-target`).textContent = lower === upper ? String(upper) : `${lower}–${upper}`;
  }
}
function readFasProfile() {
  if (fasBase.validity?.badInput) throw new Error("FAS 基础余量必须为数值");
  const overrides = JSON.parse(fasOverrides.value || "{}");
  if (!overrides || typeof overrides !== "object" || Array.isArray(overrides) || Object.hasOwn(overrides, "base"))
    throw new Error("按帧率覆盖必须是帧率到余量的对象，不包含 base");
  const hasOverrides = Object.keys(overrides).length > 0;
  const blank = fasBase.value.trim() === "";
  if (blank && !hasOverrides) return null;
  const base = blank ? fasDefaults[editingMode] : Number(fasBase.value);
  return hasOverrides ? {base, ...overrides} : base;
}
function storeDraft(settings) {
  frameSettingsProtocol(settings, seed, sceneSeed, auxSeed);
  editor.value = JSON.stringify(settings, null, 2) + "\n";
}
function updateEditMessage() {
  const error = fasEditError || otherEditError;
  message.textContent = error || "配置已编辑，点击保存并应用后生效。";
  message.dataset.error = String(Boolean(error));
}
function editFasSettings(nextMode, reset = false) {
  try {
    const settings = JSON.parse(editor.value);
    const profiles = frameFasProfiles(settings);
    profiles[editingMode] = reset ? null : readFasProfile();
    if (!reset && fasUtilLimit.validity?.badInput) throw new Error("FAS 额外下调上限必须为数值");
    const utilLimits = frameFasUtilLimits(settings);
    const limit = reset || fasUtilLimit.value.trim() === "" ? null : Number(fasUtilLimit.value);
    if (limit !== null || Object.hasOwn(settings, "fasUtilLimits")) {
      utilLimits[editingMode] = limit;
      settings.fasUtilLimits = utilLimits;
    }
    settings.fasMode = nextMode;
    delete settings.fasMarginFps;
    settings.fasProfiles = profiles;
    storeDraft(settings);
    fasEditError = "";
    showFasProfile(settings);
    showFasPreview(settings);
  } catch (error) {
    fasMode.value = editingMode;
    fasEditError = error.message;
    document.getElementById("fasStrength").hidden = true;
  }
  updateEditMessage();
}
function editOtherSettings() {
  try {
    const settings = JSON.parse(editor.value);
    if (sceneAux.checked || Object.hasOwn(settings, "sceneAux")) settings.sceneAux = Boolean(sceneAux.checked);
    if (gpuLockEdited || Object.hasOwn(settings, "sceneGpuLock")) settings.sceneGpuLock = Boolean(sceneGpuLock.checked);
    if (fallbackFps.value.trim() === "") delete settings.fallbackFps;
    else settings.fallbackFps = fallbackFps.value.trim().split(/[,，\s]+/).filter(Boolean).map(Number);
    storeDraft(settings);
    otherEditError = "";
    sceneGpuLock.disabled = busy || !sceneAux.checked;
  } catch (error) { otherEditError = error.message; }
  updateEditMessage();
}
fasMode.addEventListener("change", () => editFasSettings(fasMode.value));
for (const control of [fasBase, fasOverrides, fasUtilLimit]) control.addEventListener("change", () => editFasSettings(editingMode));
document.getElementById("fasReset").onclick = () => editFasSettings(editingMode, true);
fasPreviewFps.addEventListener("input", () => {
  try { showFasPreview(JSON.parse(editor.value)); }
  catch (_) { document.getElementById("fasStrength").hidden = true; }
});
for (const control of [fallbackFps, sceneAux]) control.addEventListener("change", editOtherSettings);
sceneGpuLock.addEventListener("change", () => { gpuLockEdited = true; editOtherSettings(); });
editor.addEventListener("input", () => {
  try {
    const settings = JSON.parse(editor.value);
    showSettings(settings);
    fasEditError = otherEditError = "";
    showFasPreview(settings);
    updateEditMessage();
  } catch (_) { document.getElementById("fasStrength").hidden = true; }
});
function execute(command) {
  return new Promise((resolve, reject) => {
    if (typeof window.ksu?.exec !== "function") return reject(new Error("请从 KernelSU 管理器打开。"));
    const name = `framePolicyReply${++counter}`;
    window[name] = (code, stdout, stderr) => {
      delete window[name];
      Number(code) === 0 ? resolve(stdout) : reject(new Error((stderr || stdout || `退出码 ${code}`).trim()));
    };
    try { window.ksu.exec(command, "{}", name); }
    catch (error) { delete window[name]; reject(error); }
  });
}
async function refresh() {
  const text = await execute(`${backend} read`), split = text.indexOf("\n");
  savedHash = text.slice(0, split).trim();
  if (!/^[a-f0-9]{64}$/.test(savedHash)) throw new Error("配置读取失败");
  editor.value = text.slice(split + 1);
  fasEditError = otherEditError = "";
  const settings = JSON.parse(editor.value);
  showSettings(settings);
  const rawStatus = await execute(`${backend} status`);
  document.getElementById("status").textContent = rawStatus;
  const status = rawStatus.replace(/\r/g, "");
  const features = (status.match(/^features=(.*)$/m)?.[1] ?? "").split(",");
  supportsStrength = features.includes("fas_mode_strength_v1");
  supportsUtilLimit = features.includes("fas_util_limit_v1");
  supportsFas = supportsStrength || features.includes("fas_tuning");
  const activeMode = Object.keys(fasLabels)[Number(status.match(/^fas_mode=(\d+)$/m)?.[1])];
  const base = status.match(/^fas_margin_base_mfps=(\d+)$/m)?.[1];
  const effective = Number(status.match(/^fas_effective_mfps=(\d+)$/m)?.[1]);
  const limit = supportsStrength || supportsUtilLimit ? status.match(/^fas_util_limit_mfps=(\d+)$/m)?.[1] : "3000";
  document.getElementById("fasUtilLimitNote").textContent = supportsUtilLimit
    ? "当前内核支持可调上限。修改后点击保存并应用，以上方内核读回值为准。"
    : "当前内核不支持可调上限。待保存预览中的自定义上限需更新内核才能应用。";
  const liveStatus = document.getElementById("fasLiveStatus");
  if (activeMode && /^configured=1$/m.test(status)) {
    const owner = status.match(/^owner=(\w+)$/m)?.[1];
    const observing = /^observe=1$/m.test(status);
    const frameActive = owner === "frame" && /^probe=1$/m.test(status) && !observing;
    const details = [`内核已载入${fasLabels[activeMode]}档。`];
    if (base !== undefined) details.push(`基础余量为 ${Number(base) / 1000} FPS。`);
    if (limit !== undefined) details.push(`最多再下调 ${Number(limit) / 1000} FPS。`);
    if (observing) details.push("当前仅观察。");
    else if (owner === "scene") details.push("当前由 Scene 调度。");
    else if (frameActive) {
      details.push("当前由游戏 FAS 调度。");
      if (effective > 0) details.push(`最近帧的内部目标为 ${effective / 1000} FPS。`);
    }
    liveStatus.textContent = details.join("");
  } else liveStatus.textContent = "当前未报告已载入的 FAS 档位。";
  document.getElementById("fasStrengthNote").textContent = supportsStrength
    ? "60 FPS 起按基础值扣减，低于 60 FPS 按比例折算。"
    : supportsUtilLimit
      ? "基础余量按「帧率 ÷ 60」折算。额外下调使用本档上限，留空时为 3 FPS。"
    : supportsFas
      ? "当前内核使用旧版 FAS。高帧率档位按「帧率 ÷ 60」放大基础余量，自适应最多再下调 3 FPS。"
      : "当前状态未报告 FAS 调优能力。";
  showFasPreview(settings);
}
async function run(task) {
  if (busy) return;
  busy = true;
  formControls.forEach(control => control.disabled = true);
  message.textContent = ""; message.dataset.error = "false";
  try { await task(); }
  catch (error) { message.textContent = error.message; message.dataset.error = "true"; }
  finally {
    busy = false;
    formControls.forEach(control => control.disabled = false);
    sceneGpuLock.disabled = !sceneAux.checked;
    fasUtilLimit.disabled = !supportsUtilLimit;
  }
}
document.getElementById("save").onclick = () => run(async () => {
  if (fasBase.validity?.badInput || fasUtilLimit.validity?.badInput || fasBase.value !== displayedBase ||
      fasOverrides.value !== displayedOverrides || fasUtilLimit.value !== displayedUtilLimit)
    editFasSettings(editingMode);
  if (fasEditError || otherEditError) throw new Error(fasEditError || otherEditError);
  const settings = JSON.parse(editor.value);
  if (frameFasProtocol(settings).utilLimit !== null && !supportsUtilLimit)
    throw new Error("当前内核不支持可调下调上限，未保存；请先更新内核");
  const {payload} = frameSettingsProtocol(settings, seed, sceneSeed, auxSeed);
  let binary = "";
  for (let i = 0; i < payload.length; i += 8192) binary += String.fromCharCode(...payload.subarray(i, i + 8192));
  await execute(`printf %s '${btoa(binary)}' | base64 -d | ${backend} save ${savedHash}`);
  await refresh(); message.textContent = "已保存并应用。内核状态显示在上方。";
});
document.getElementById("refresh").onclick = () => run(refresh);
document.getElementById("rollback").onclick = () => run(async () => {
  await execute(`${backend} rollback`); await refresh(); message.textContent = "已恢复上次配置。";
});
run(async () => {
  seed = JSON.parse(await execute(`${backend} seed`));
  sceneSeed = JSON.parse(await execute(`${backend} scene-seed`));
  auxSeed = JSON.parse(await execute(`${backend} aux-seed`));
  await refresh();
});
