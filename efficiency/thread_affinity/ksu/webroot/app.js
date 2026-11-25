"use strict";

const backend = "sh /data/adb/modules/ace6_thread_affinity/config.sh";
const editor = document.getElementById("rules-editor");
const feedback = document.getElementById("feedback");
const save = document.getElementById("save");
const refresh = document.getElementById("refresh");
const importButton = document.getElementById("import");
const previous = document.getElementById("previous");
const buttons = [save, refresh, importButton, previous];
let requestId = 0;
let hash = "";
let savedText = "";
let busy = false;

function exec(command) {
  return new Promise((resolve, reject) => {
    if (typeof window.ksu?.exec !== "function") {
      reject(new Error("请从 KernelSU 管理器打开此界面。"));
      return;
    }
    const callback = `ace6AffinityReply${++requestId}`;
    window[callback] = (code, stdout, stderr) => {
      delete window[callback];
      if (Number(code) === 0) resolve(stdout);
      else reject(new Error((stderr || stdout || `命令退出 ${code}`).trim()));
    };
    try {
      window.ksu.exec(command, "{}", callback);
    } catch (error) {
      delete window[callback];
      reject(error);
    }
  });
}

function updateEditor() {
  const lines = editor.value.split("\n").filter(line => line.trim() && !line.trimStart().startsWith("#"));
  document.getElementById("line-count").textContent = `${lines.length} 条`;
  editor.disabled = busy || !hash;
  buttons.forEach(button => { button.disabled = busy; });
  save.disabled = busy || !hash || editor.value === savedText;
  importButton.disabled = busy || !hash;
  previous.disabled = busy || !hash;
}

function showStatus(text) {
  const status = Object.fromEntries(text.trim().split("\n").map(line => line.split("=")));
  document.getElementById("rules").textContent = status.rules ?? "—";
  document.getElementById("foreground").textContent = status.foreground === "-" ? "—" : (status.foreground ?? "—");
}

async function readEditor() {
  const response = await exec(`${backend} read`);
  const separator = response.indexOf("\n");
  const nextHash = response.slice(0, separator).trim();
  if (separator < 0 || !/^[a-f0-9]{64}$/.test(nextHash)) throw new Error("无法读取规则文件。");
  hash = nextHash;
  editor.value = response.slice(separator + 1);
  savedText = editor.value;
}

async function run(task) {
  if (busy) return;
  busy = true;
  feedback.textContent = "";
  feedback.dataset.error = "false";
  updateEditor();
  try {
    await task();
  } catch (error) {
    feedback.textContent = error.message;
    feedback.dataset.error = "true";
  } finally {
    busy = false;
    updateEditor();
  }
}

function replaceDraft() {
  return editor.value === savedText || window.confirm("放弃当前未保存的修改？");
}

async function refreshAll() {
  await readEditor();
  showStatus(await exec(`${backend} status`));
}

editor.addEventListener("input", updateEditor);
refresh.addEventListener("click", () => { if (replaceDraft()) run(refreshAll); });
importButton.addEventListener("click", () => {
  if (!replaceDraft()) return;
  run(async () => {
    editor.value = await exec(`${backend} import`);
    feedback.textContent = "已读入 AppOpt 规则。保存后生效。";
  });
});
previous.addEventListener("click", () => {
  if (!replaceDraft()) return;
  run(async () => {
    editor.value = await exec(`${backend} previous`);
    feedback.textContent = "已读入上次备份。保存后生效。";
  });
});
save.addEventListener("click", () => run(async () => {
  const bytes = new TextEncoder().encode(editor.value);
  if (bytes.length > 65536) throw new Error("手写规则文件不能超过 64 KiB。");
  let binary = "";
  for (let start = 0; start < bytes.length; start += 16384) {
    binary += String.fromCharCode(...bytes.subarray(start, start + 16384));
  }
  const encoded = btoa(binary);
  const status = await exec(`printf %s '${encoded}' | base64 -d | ${backend} save ${hash}`);
  showStatus(status);
  await readEditor();
  feedback.textContent = "已保存并应用。";
}));

run(refreshAll);
