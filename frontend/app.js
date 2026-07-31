"use strict";

const invoke = window.__TAURI__?.core?.invoke;

const TARGETS = [
  {
    id: "taskbar",
    name: "Taskbar",
    icon: "i-taskbar",
    description: "Independent color, clock, opacity, notification center, control center, and Show desktop."
  },
  {
    id: "start",
    name: "Start menu",
    icon: "i-start",
    description: "Independent Start surface color, opacity, and Recommended-section visibility."
  },
  {
    id: "explorer",
    name: "File Explorer",
    icon: "i-explorer",
    description: "Independent full-window color and ownership-aware titles for Explorer folder windows."
  }
];

const ROUTE_TITLES = {
  overview: "Overview",
  customizations: "Customizations",
  taskbar: "Taskbar",
  start: "Start menu",
  explorer: "File Explorer",
  diagnostics: "Diagnostics",
  about: "About"
};

const STATE_LABELS = {
  disabled: "Disabled",
  stopped: "Stopped",
  running: "Running",
  injecting: "Injecting",
  active: "Active",
  error: "Error",
  incompatible: "Incompatible"
};

const elements = {
  content: document.querySelector("#content"),
  currentView: document.querySelector("#current-view"),
  connectionChip: document.querySelector("#connection-chip"),
  connectionText: document.querySelector("#connection-text"),
  overviewTargets: document.querySelector("#overview-targets"),
  targetList: document.querySelector("#target-list"),
  diagnosticsTargets: document.querySelector("#diagnostic-targets"),
  activeCount: document.querySelector("#overview-active-count"),
  overviewSummary: document.querySelector("#overview-summary"),
  refreshDiagnostics: document.querySelector("#refresh-diagnostics"),
  xamlSummary: document.querySelector("#xaml-summary"),
  xamlTypes: document.querySelector("#xaml-types"),
  toastRegion: document.querySelector("#toast-region")
};

const runtime = {
  route: "overview",
  state: null,
  refreshing: false,
  pending: new Set(),
  pollTimer: 0
};

function icon(id) {
  return `<svg aria-hidden="true"><use href="#${id}"></use></svg>`;
}

function buildStaticCards() {
  elements.overviewTargets.innerHTML = TARGETS.map((target) => `
    <button class="overview-card" type="button" data-open-target="${target.id}">
      <span class="overview-card-top">
        <span class="overview-card-icon">${icon(target.icon)}</span>
        <span class="mini-state"><span class="status-bar" data-target-bar="${target.id}"></span><span data-target-state="${target.id}">Unknown</span></span>
      </span>
      <h3>${target.name}</h3>
      <p>${target.description}</p>
    </button>`).join("");

  elements.targetList.innerHTML = TARGETS.map((target) => `
    <article class="target-card" data-target-card="${target.id}">
      <span class="target-card-icon">${icon(target.icon)}</span>
      <div class="target-card-copy"><strong>${target.name}</strong><p>${target.description}</p></div>
      <div class="target-state"><span class="status-bar" data-target-bar="${target.id}"></span><span data-target-state="${target.id}">Unknown</span></div>
      <label class="switch"><input type="checkbox" data-target-toggle="${target.id}" aria-label="Enable ${target.name} customization"><span></span></label>
      <button class="open-target" type="button" data-open-target="${target.id}" aria-label="Open ${target.name}">${icon("i-chevron")}</button>
    </article>`).join("");

  elements.diagnosticsTargets.innerHTML = TARGETS.map((target) => `
    <article class="diagnostic-card" data-diagnostic-card="${target.id}">
      <div class="diagnostic-card-header"><strong>${target.name}</strong><span class="mini-state"><span class="status-bar" data-target-bar="${target.id}"></span><span data-target-state="${target.id}">Unknown</span></span></div>
      <dl class="diagnostic-facts">
        <dt>Process</dt><dd data-diagnostic="process">Not detected</dd>
        <dt>Native agent</dt><dd data-diagnostic="agent">Not loaded</dd>
        <dt>Process ID</dt><dd data-diagnostic="pid">—</dd>
        <dt>Host detail</dt><dd data-diagnostic="detail">Waiting</dd>
      </dl>
    </article>`).join("");
}

function navigate(route) {
  if (!ROUTE_TITLES[route]) return;
  runtime.route = route;
  document.querySelectorAll(".nav-item[data-route]").forEach((button) => {
    button.classList.toggle("is-active", button.dataset.route === route);
  });
  document.querySelectorAll(".page[data-page]").forEach((page) => {
    page.classList.toggle("is-active", page.dataset.page === route);
  });
  elements.currentView.textContent = ROUTE_TITLES[route];
  elements.content.scrollTop = 0;
  if (route === "diagnostics") updateDiagnosticCards();
}

function bindNavigation() {
  document.addEventListener("click", (event) => {
    const routeButton = event.target.closest("[data-route]");
    const targetButton = event.target.closest("[data-open-target]");
    if (routeButton) navigate(routeButton.dataset.route);
    if (targetButton) navigate(targetButton.dataset.openTarget);
  });
}

function bindControls() {
  document.querySelectorAll("[data-target-toggle]").forEach((control) => {
    control.addEventListener("change", () => toggleTarget(control));
  });

  document.querySelectorAll("[data-custom-toggle]").forEach((control) => {
    control.addEventListener("change", async () => {
      const key = `custom:${control.dataset.customToggle}`;
      await runPending(key, async () => {
        await invokeCommand("set_customization", {
          request: { id: control.dataset.customToggle, booleanValue: control.checked }
        });
      });
    });
  });

  document.querySelectorAll("[data-policy-toggle]").forEach((control) => {
    control.addEventListener("change", async () => {
      const key = `policy:${control.dataset.policyToggle}`;
      await runPending(key, async () => {
        await invokeCommand("set_start_all_apps_hidden", { hidden: control.checked });
      });
    });
  });

  bindRange("taskbar-opacity", "taskbarOpacity");
  bindRange("start-opacity", "startOpacity");

  bindColorControl("taskbar", "taskbarBackgroundColor");
  bindColorControl("start", "startBackgroundColor");
  bindColorControl("explorer", "explorerBackgroundColor");

  document.querySelectorAll("[data-transition-animation]").forEach((button) => {
    button.addEventListener("click", async () => {
      const animation = Number(button.dataset.transitionAnimation);
      if (!Number.isInteger(animation) || animation < 0 || animation > 3) return;
      await runPending("custom:explorerTransitionAnimation", async () => {
        await invokeCommand("set_customization", {
          request: { id: "explorerTransitionAnimation", integerValue: animation }
        });
      });
    });
  });

  document.querySelectorAll("[data-range-step]").forEach((button) => {
    button.addEventListener("click", () => {
      const range = document.getElementById(button.dataset.rangeStep);
      const next = Number(range.value) + Number(button.dataset.delta);
      range.value = String(Math.max(Number(range.min), Math.min(Number(range.max), next)));
      range.dispatchEvent(new Event("input", { bubbles: true }));
      range.dispatchEvent(new Event("change", { bubbles: true }));
    });
  });

  document.querySelectorAll("[data-text-form]").forEach((form) => {
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const input = form.querySelector("input");
      const key = `custom:${form.dataset.textForm}`;
      await runPending(key, async () => {
        await invokeCommand("set_customization", {
          request: { id: form.dataset.textForm, textValue: input.value }
        });
      });
    });
  });

  elements.refreshDiagnostics.addEventListener("click", refreshXamlDiagnostics);
}

function bindColorControl(target, customizationId) {
  const picker = document.querySelector(`[data-color-picker="${target}"]`);
  const hex = document.querySelector(`[data-color-hex="${target}"]`);
  const apply = document.querySelector(`[data-color-apply="${target}"]`);
  if (!picker || !hex || !apply) return;

  picker.addEventListener("input", () => {
    hex.value = picker.value.toUpperCase();
    hex.setCustomValidity("");
  });
  hex.addEventListener("input", () => {
    const normalized = normalizeHexColor(hex.value);
    hex.setCustomValidity(normalized ? "" : "Use a color in #RRGGBB format");
    if (normalized) picker.value = normalized;
  });
  apply.addEventListener("click", async () => {
    const normalized = normalizeHexColor(hex.value);
    if (!normalized) {
      hex.setCustomValidity("Use a color in #RRGGBB format");
      hex.reportValidity();
      return;
    }
    hex.value = normalized.toUpperCase();
    const argb = 0xFF000000 + Number.parseInt(normalized.slice(1), 16);
    await runPending(`custom:${customizationId}`, async () => {
      await invokeCommand("set_customization", {
        request: { id: customizationId, integerValue: argb }
      });
    });
  });
}

function normalizeHexColor(value) {
  const trimmed = String(value || "").trim();
  const withHash = trimmed.startsWith("#") ? trimmed : `#${trimmed}`;
  return /^#[0-9a-fA-F]{6}$/.test(withHash) ? withHash.toLowerCase() : null;
}

function bindRange(elementId, customizationId) {
  const range = document.getElementById(elementId);
  const output = document.querySelector(`output[for="${elementId}"]`);
  const updateVisual = () => {
    const minimum = Number(range.min);
    const maximum = Number(range.max);
    const value = Number(range.value);
    output.value = `${value}%`;
    range.style.setProperty("--range-progress", `${((value - minimum) / (maximum - minimum)) * 100}%`);
  };
  range.addEventListener("input", updateVisual);
  range.addEventListener("change", async () => {
    const key = `custom:${customizationId}`;
    await runPending(key, async () => {
      await invokeCommand("set_customization", {
        request: { id: customizationId, integerValue: Number(range.value) }
      });
    });
  });
  updateVisual();
}

async function toggleTarget(control) {
  const target = control.dataset.targetToggle;
  const key = `target:${target}`;
  const enabled = control.checked;
  await runPending(key, async () => {
    await invokeCommand("set_target_enabled", {
      request: { target, enabled, confirmed: enabled }
    });
  });
}

async function runPending(key, action) {
  if (!invoke || runtime.pending.has(key)) return;
  runtime.pending.add(key);
  updateControlAvailability();
  try {
    await action();
    await refreshState(true);
  } catch (error) {
    showToast(readError(error), true);
    await refreshState(true);
  } finally {
    runtime.pending.delete(key);
    updateControlAvailability();
  }
}

async function invokeCommand(command, payload) {
  const result = await invoke(command, payload);
  if (!result?.accepted) throw new Error(result?.detail || "The native host rejected the command");
  if (result.detail) showToast(result.detail, false);
  return result;
}

async function refreshState(force = false) {
  if (!invoke || runtime.refreshing || (document.hidden && !force)) return;
  runtime.refreshing = true;
  try {
    const state = await invoke("get_app_state");
    runtime.state = state;
    applyState(state);
  } catch (error) {
    applyConnection(false, readError(error));
  } finally {
    runtime.refreshing = false;
  }
}

function applyState(state) {
  applyConnection(state.connected, state.connectionDetail);
  const targets = new Map((state.targets || []).map((target) => [target.id, target]));

  for (const metadata of TARGETS) {
    const target = targets.get(metadata.id) || {
      id: metadata.id,
      state: "stopped",
      enabled: false,
      processRunning: false,
      agentLoaded: false,
      processId: 0,
      detail: "No host snapshot"
    };
    const label = STATE_LABELS[target.state] || target.state || "Unknown";
    document.querySelectorAll(`[data-target-state="${metadata.id}"]`).forEach((node) => { node.textContent = label; });
    document.querySelectorAll(`[data-state-label="${metadata.id}"]`).forEach((node) => { node.textContent = label; });
    document.querySelectorAll(`[data-target-bar="${metadata.id}"]`).forEach((bar) => {
      bar.classList.toggle("is-active", target.state === "active");
      bar.style.backgroundColor = target.state === "error" || target.state === "incompatible" ? "var(--danger)" : "";
    });
    document.querySelectorAll(`[data-meter="${metadata.id}"]`).forEach((meter) => meter.classList.toggle("is-running", target.state === "active"));
    document.querySelectorAll(`[data-target-toggle="${metadata.id}"]`).forEach((control) => {
      if (!runtime.pending.has(`target:${metadata.id}`)) control.checked = Boolean(target.enabled);
    });
  }

  const active = [...targets.values()].filter((target) => target.state === "active").length;
  const enabled = [...targets.values()].filter((target) => target.enabled).length;
  elements.activeCount.textContent = state.connected ? `${active} of 3 targets active` : "Native host unavailable";
  elements.overviewSummary.textContent = state.connected
    ? `${enabled} enabled · ${active} injected and responding`
    : state.connectionDetail;

  applySettings(state.settings || {});
  updateDiagnosticCards();
  updateControlAvailability();
}

function applyConnection(connected, detail) {
  elements.connectionChip.classList.toggle("is-connected", connected);
  elements.connectionChip.classList.toggle("is-error", !connected);
  elements.connectionChip.classList.remove("is-waiting");
  elements.connectionText.textContent = connected ? "Host connected" : "Host unavailable";
  elements.connectionChip.title = detail || elements.connectionText.textContent;
}

function applySettings(settings) {
  setInputValue("taskbar-opacity", settings.taskbarOpacity, true);
  setInputValue("start-opacity", settings.startMenuOpacity, true);
  setInputValue("clock-prefix", settings.taskbarClockPrefix, false);
  setInputValue("explorer-prefix", settings.fileExplorerTitlePrefix, false);
  setColorValue("taskbar", settings.taskbarBackgroundColor);
  setColorValue("start", settings.startMenuBackgroundColor);
  setColorValue("explorer", settings.fileExplorerBackgroundColor);
  document.querySelectorAll("[data-transition-animation]").forEach((button) => {
    const selected = Number(button.dataset.transitionAnimation) ===
      Number(settings.fileExplorerTransitionAnimation ?? 1);
    button.classList.toggle("is-active", selected);
    button.setAttribute("aria-pressed", String(selected));
  });

  const toggles = {
    taskbarHideNotificationCenter: settings.taskbarHideNotificationCenter,
    taskbarHideControlCenter: settings.taskbarHideControlCenter,
    taskbarHideShowDesktop: settings.taskbarHideShowDesktop,
    taskbarCapsuleEnabled: settings.taskbarCapsuleEnabled,
    startHideRecommended: settings.startMenuHideRecommended,
    taskbarBackgroundColorEnabled: settings.taskbarBackgroundColorEnabled,
    explorerBackgroundColorEnabled: settings.fileExplorerBackgroundColorEnabled,
    explorerCustomScrollbarEnabled: settings.fileExplorerCustomScrollbarEnabled,
    startBackgroundColorEnabled: settings.startMenuBackgroundColorEnabled
  };
  Object.entries(toggles).forEach(([id, checked]) => {
    const control = document.querySelector(`[data-custom-toggle="${id}"]`);
    if (control && !runtime.pending.has(`custom:${id}`)) control.checked = Boolean(checked);
  });

  const allAppsToggle = document.querySelector('[data-policy-toggle="startHideAllApps"]');
  if (allAppsToggle && !runtime.pending.has("policy:startHideAllApps")) {
    allAppsToggle.checked = Boolean(settings.startMenuHideAllApps);
  }
  const allAppsDetail = document.querySelector("#start-all-apps-detail");
  if (allAppsDetail) {
    allAppsDetail.textContent = settings.startMenuHideAllAppsDetail ||
      "Remove every app entry from the All section and leave that area empty. Requires administrator approval.";
  }
}

function setColorValue(target, argb) {
  if (!Number.isInteger(argb)) return;
  const rgb = `#${(argb >>> 0).toString(16).padStart(8, "0").slice(-6)}`;
  const picker = document.querySelector(`[data-color-picker="${target}"]`);
  const hex = document.querySelector(`[data-color-hex="${target}"]`);
  if (picker && document.activeElement !== picker) picker.value = rgb;
  if (hex && document.activeElement !== hex) {
    hex.value = rgb.toUpperCase();
    hex.setCustomValidity("");
  }
}

function setInputValue(id, value, dispatchInput) {
  const control = document.getElementById(id);
  if (!control || value === undefined || document.activeElement === control) return;
  control.value = String(value);
  if (dispatchInput) control.dispatchEvent(new Event("input"));
}

function updateControlAvailability() {
  const connected = Boolean(runtime.state?.connected);
  document.querySelectorAll("[data-target-toggle]").forEach((control) => {
    control.disabled = !connected || runtime.pending.has(`target:${control.dataset.targetToggle}`);
  });
  document.querySelectorAll("[data-custom-toggle]").forEach((control) => {
    const id = control.dataset.customToggle;
    const requiresExplorerColor = id === "explorerCustomScrollbarEnabled";
    control.disabled = !connected || runtime.pending.has(`custom:${id}`) ||
      (requiresExplorerColor && !runtime.state?.settings?.fileExplorerBackgroundColorEnabled);
    if (requiresExplorerColor) {
      control.title = control.disabled && connected
        ? "Enable the custom File Explorer color first"
        : "";
    }
  });
  document.querySelectorAll("[data-policy-toggle]").forEach((control) => {
    const pending = runtime.pending.has(`policy:${control.dataset.policyToggle}`);
    const settings = runtime.state?.settings;
    const available = Boolean(
      settings?.startMenuHideAllAppsSupported && settings?.startMenuHideAllAppsEditable
    );
    control.disabled = pending || !available;
    control.title = settings?.startMenuHideAllAppsDetail || "Windows policy unavailable";
  });
  document.querySelectorAll("[data-text-form]").forEach((form) => {
    const pending = runtime.pending.has(`custom:${form.dataset.textForm}`);
    form.querySelectorAll("input, button").forEach((control) => { control.disabled = !connected || pending; });
  });
  const colorMap = {
    taskbar: "taskbarBackgroundColor",
    start: "startBackgroundColor",
    explorer: "explorerBackgroundColor"
  };
  Object.entries(colorMap).forEach(([target, customization]) => {
    const pending = runtime.pending.has(`custom:${customization}`);
    document.querySelectorAll(
      `[data-color-picker="${target}"], [data-color-hex="${target}"], [data-color-apply="${target}"]`
    ).forEach((control) => { control.disabled = !connected || pending; });
  });
  const rangeMap = { "taskbar-opacity": "taskbarOpacity", "start-opacity": "startOpacity" };
  Object.entries(rangeMap).forEach(([id, customization]) => {
    const pending = runtime.pending.has(`custom:${customization}`);
    document.getElementById(id).disabled = !connected || pending;
    document.querySelectorAll(`[data-range-step="${id}"]`).forEach((button) => { button.disabled = !connected || pending; });
  });
  const transitionPending = runtime.pending.has("custom:explorerTransitionAnimation");
  document.querySelectorAll("[data-transition-animation]").forEach((button) => {
    button.disabled = !connected || transitionPending;
  });
}

function updateDiagnosticCards() {
  const targets = new Map((runtime.state?.targets || []).map((target) => [target.id, target]));
  for (const metadata of TARGETS) {
    const card = document.querySelector(`[data-diagnostic-card="${metadata.id}"]`);
    const target = targets.get(metadata.id);
    if (!card || !target) continue;
    card.querySelector('[data-diagnostic="process"]').textContent = target.processRunning ? "Running" : "Not detected";
    card.querySelector('[data-diagnostic="agent"]').textContent = target.agentLoaded ? "Loaded" : "Not loaded";
    card.querySelector('[data-diagnostic="pid"]').textContent = target.processId ? String(target.processId) : "—";
    card.querySelector('[data-diagnostic="detail"]').textContent = target.detail || "—";
    card.querySelector('[data-diagnostic="detail"]').title = target.detail || "";
  }
}

async function refreshXamlDiagnostics() {
  if (!invoke || !runtime.state?.connected) {
    showToast("The native host is not connected.", true);
    return;
  }
  elements.refreshDiagnostics.disabled = true;
  try {
    const diagnostics = await invoke("get_xaml_diagnostics");
    elements.xamlSummary.textContent = `${diagnostics.trackedElementCount} tracked · ${diagnostics.droppedTypeCount} dropped types · ${diagnostics.droppedElementCount} dropped elements`;
    elements.xamlTypes.replaceChildren();
    if (!diagnostics.types.length) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      cell.colSpan = 2;
      cell.className = "empty-cell";
      cell.textContent = "No XAML types have been observed yet.";
      row.append(cell);
      elements.xamlTypes.append(row);
    } else {
      diagnostics.types.forEach((type) => {
        const row = document.createElement("tr");
        const name = document.createElement("td");
        const count = document.createElement("td");
        name.textContent = type.typeName;
        count.textContent = String(type.observationCount);
        row.append(name, count);
        elements.xamlTypes.append(row);
      });
    }
  } catch (error) {
    showToast(readError(error), true);
  } finally {
    elements.refreshDiagnostics.disabled = false;
  }
}

function readError(error) {
  if (typeof error === "string") return error;
  if (error?.message) return error.message;
  return "An unexpected local control error occurred.";
}

function showToast(message, isError) {
  const toast = document.createElement("div");
  toast.className = `toast${isError ? " is-error" : ""}`;
  const text = document.createElement("span");
  text.textContent = message;
  toast.append(text);
  elements.toastRegion.append(toast);
  window.setTimeout(() => {
    toast.classList.add("is-leaving");
    window.setTimeout(() => toast.remove(), 180);
  }, isError ? 5200 : 3000);
}

function schedulePolling() {
  window.clearInterval(runtime.pollTimer);
  runtime.pollTimer = window.setInterval(() => refreshState(false), 1200);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) refreshState(true);
  });
}

function start() {
  buildStaticCards();
  bindNavigation();
  bindControls();
  updateControlAvailability();
  if (!invoke) {
    applyConnection(false, "Tauri IPC is unavailable");
    elements.activeCount.textContent = "Tauri IPC unavailable";
    elements.overviewSummary.textContent = "Run the bundled desktop application to connect to the native host.";
    return;
  }
  refreshState(true);
  schedulePolling();
}

start();
