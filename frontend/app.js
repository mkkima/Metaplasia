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
  "notifications-audio": "Notifications & audio",
  diagnostics: "Diagnostics",
  about: "About"
};

const DEMO_STORAGE_KEY = "metaplasia.notifications-audio-demo.v1";
const UPDATE_STORAGE_KEY = "metaplasia.portable-updates.automatic.v1";
const UPDATE_CHECK_INTERVAL_MS = 6 * 60 * 60 * 1000;
const DEMO_DEFAULTS = Object.freeze({
  notificationPosition: "top",
  notificationDensity: "comfortable",
  notificationActions: true,
  notificationDuration: 6,
  audioLayout: "expanded",
  showOutputDevice: true,
  showMediaControls: true,
  volume: 68,
  muted: false
});

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
  toastRegion: document.querySelector("#toast-region"),
  demoDesktop: document.querySelector("[data-demo-desktop]"),
  notificationPreview: document.querySelector("[data-notification-preview]"),
  audioPreview: document.querySelector("[data-audio-preview]"),
  applicationVersion: document.querySelector("#application-version"),
  automaticUpdates: document.querySelector("#automatic-updates"),
  checkUpdates: document.querySelector("#check-updates"),
  downloadUpdate: document.querySelector("#download-update"),
  installUpdate: document.querySelector("#install-update"),
  updateStatusBar: document.querySelector("#update-status-bar"),
  updateStatusTitle: document.querySelector("#update-status-title"),
  updateStatusDetail: document.querySelector("#update-status-detail"),
  updateNotes: document.querySelector("#update-notes"),
  updateConfirmDialog: document.querySelector("#update-confirm-dialog")
};

const runtime = {
  route: "overview",
  state: null,
  refreshing: false,
  pending: new Set(),
  pollTimer: 0,
  demo: loadDemoSettings(),
  demoNotificationTimer: 0,
  demoMediaPlaying: false,
  updateStatus: null,
  updateBusy: false,
  updateTimer: 0
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
        if (control.dataset.customToggle === "startThreePanelLayoutEnabled" &&
            control.checked && runtime.state?.settings?.startMenuHideAllAppsPolicyActive) {
          await invokeCommand("set_start_all_apps_hidden", {
            hidden: true,
            threePanelEnabled: true
          });
        }
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
        await invokeCommand("set_start_all_apps_hidden", {
          hidden: control.checked,
          threePanelEnabled: Boolean(
            runtime.state?.settings?.startMenuThreePanelLayoutEnabled
          )
        });
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

  document.querySelectorAll("[data-demo-choice]").forEach((button) => {
    button.addEventListener("click", () => {
      updateDemoSetting(button.dataset.demoChoice, button.dataset.demoValue);
    });
  });

  document.querySelectorAll("[data-demo-toggle]").forEach((control) => {
    control.addEventListener("change", () => {
      updateDemoSetting(control.dataset.demoToggle, control.checked);
    });
  });

  document.querySelectorAll("[data-demo-range]").forEach((control) => {
    control.addEventListener("input", () => {
      updateDemoSetting(control.dataset.demoRange, Number(control.value));
    });
  });

  document.querySelectorAll("[data-demo-volume]").forEach((control) => {
    control.addEventListener("input", () => {
      runtime.demo.muted = false;
      updateDemoSetting("volume", Number(control.value));
    });
  });

  document.querySelectorAll("[data-demo-range-step]").forEach((button) => {
    button.addEventListener("click", () => {
      const range = document.getElementById(button.dataset.demoRangeStep);
      if (!range) return;
      const next = Number(range.value) + Number(button.dataset.delta);
      range.value = String(Math.max(Number(range.min), Math.min(Number(range.max), next)));
      range.dispatchEvent(new Event("input", { bubbles: true }));
    });
  });

  document.querySelectorAll("[data-demo-volume-mute]").forEach((button) => {
    button.addEventListener("click", () => updateDemoSetting("muted", !runtime.demo.muted));
  });

  document.querySelector("[data-demo-show-notification]")?.addEventListener("click", showDemoNotification);
  document.querySelector("[data-demo-show-audio]")?.addEventListener("click", showDemoAudio);
  document.querySelectorAll("[data-demo-dismiss-notification]").forEach((button) => {
    button.addEventListener("click", dismissDemoNotification);
  });
  document.querySelector("[data-demo-reset]")?.addEventListener("click", resetDemoSettings);
  document.querySelector("[data-demo-notification-open]")?.addEventListener("click", () => {
    showToast("Demo action only — no Windows notification was opened.", false);
  });
  document.querySelector("[data-demo-device]")?.addEventListener("click", () => {
    showToast("Output device picker is part of the UI demo.", false);
  });
  document.querySelector("[data-demo-media-toggle]")?.addEventListener("click", toggleDemoMedia);

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

  elements.automaticUpdates?.addEventListener("change", () => {
    saveAutomaticUpdates(elements.automaticUpdates.checked);
    if (elements.automaticUpdates.checked) checkForPortableUpdate(true);
  });
  elements.checkUpdates?.addEventListener("click", () => checkForPortableUpdate(false));
  elements.downloadUpdate?.addEventListener("click", downloadPortableUpdate);
  elements.installUpdate?.addEventListener("click", () => {
    if (!runtime.updateBusy) elements.updateConfirmDialog?.showModal();
  });
  elements.updateConfirmDialog?.addEventListener("close", () => {
    if (elements.updateConfirmDialog.returnValue === "install") installPortableUpdate();
  });
}

function loadAutomaticUpdates() {
  try {
    const stored = window.localStorage.getItem(UPDATE_STORAGE_KEY);
    return stored === null ? true : stored === "1";
  } catch {
    return true;
  }
}

function saveAutomaticUpdates(enabled) {
  try {
    window.localStorage.setItem(UPDATE_STORAGE_KEY, enabled ? "1" : "0");
  } catch {
    // A hardened WebView profile can reject storage. The current-session
    // setting still works and remains safe because installation is explicit.
  }
}

function updatePhaseTitle(status) {
  const version = status?.availableVersion;
  switch (status?.phase) {
    case "unconfigured": return "Update channel is not configured in this build";
    case "checking": return "Checking for a signed portable update…";
    case "current": return "Metaplasia is up to date";
    case "available": return version ? `Metaplasia ${version} is available` : "An update is available";
    case "downloading": return version ? `Downloading Metaplasia ${version}…` : "Downloading update…";
    case "ready": return version ? `Metaplasia ${version} is ready` : "Update is ready";
    case "installing": return "Installing the verified portable update…";
    case "error": return "Portable update failed";
    default: return "Portable update channel";
  }
}

function renderUpdateStatus(status = runtime.updateStatus) {
  if (!status || !elements.updateStatusTitle) return;
  runtime.updateStatus = status;
  elements.applicationVersion.textContent = `Metaplasia ${status.currentVersion}`;
  elements.updateStatusTitle.textContent = updatePhaseTitle(status);
  elements.updateStatusDetail.textContent = status.detail || "Ready to check GitHub Releases.";
  elements.updateNotes.textContent = status.notes || "";
  elements.updateNotes.hidden = !status.notes;
  const isError = status.phase === "error";
  const isPositive = ["current", "available", "ready"].includes(status.phase);
  elements.updateStatusBar.classList.toggle("is-error", isError);
  elements.updateStatusBar.classList.toggle("is-active", isPositive);

  const enabled = Boolean(status.configured) && !runtime.updateBusy;
  elements.checkUpdates.disabled = !enabled;
  elements.downloadUpdate.hidden = status.phase !== "available" || status.downloaded;
  elements.downloadUpdate.disabled = !enabled;
  elements.installUpdate.hidden = status.phase !== "ready" || !status.downloaded;
  elements.installUpdate.disabled = !enabled;
  elements.automaticUpdates.disabled = !status.configured || runtime.updateBusy;
}

async function checkForPortableUpdate(automatic) {
  if (!invoke || runtime.updateBusy) return;
  runtime.updateBusy = true;
  renderUpdateStatus({
    ...(runtime.updateStatus || {}),
    configured: runtime.updateStatus?.configured !== false,
    currentVersion: runtime.updateStatus?.currentVersion || "",
    phase: "checking",
    detail: "Downloading and verifying the signed release manifest."
  });
  try {
    let status = await invoke("check_portable_update");
    runtime.updateStatus = status;
    if (status.availableVersion && !status.downloaded &&
        (automatic || elements.automaticUpdates.checked)) {
      status = await invoke("download_portable_update");
    }
    runtime.updateStatus = status;
  } catch (error) {
    runtime.updateStatus = {
      ...(runtime.updateStatus || {}),
      configured: true,
      phase: "error",
      detail: readError(error),
      notes: "",
      downloaded: false
    };
    if (!automatic) showToast(readError(error), true);
  } finally {
    runtime.updateBusy = false;
    renderUpdateStatus();
  }
}

async function downloadPortableUpdate() {
  if (!invoke || runtime.updateBusy) return;
  runtime.updateBusy = true;
  renderUpdateStatus({
    ...(runtime.updateStatus || {}),
    phase: "downloading",
    detail: "Downloading and verifying the complete portable package."
  });
  try {
    runtime.updateStatus = await invoke("download_portable_update");
  } catch (error) {
    runtime.updateStatus = {
      ...(runtime.updateStatus || {}),
      phase: "error",
      detail: readError(error)
    };
    showToast(readError(error), true);
  } finally {
    runtime.updateBusy = false;
    renderUpdateStatus();
  }
}

async function installPortableUpdate() {
  if (!invoke || runtime.updateBusy || runtime.updateStatus?.phase !== "ready") return;
  runtime.updateBusy = true;
  renderUpdateStatus({
    ...runtime.updateStatus,
    phase: "installing",
    detail: "Stopping local components and starting the portable update helper."
  });
  try {
    await invoke("apply_portable_update");
  } catch (error) {
    runtime.updateBusy = false;
    runtime.updateStatus = {
      ...runtime.updateStatus,
      phase: "error",
      detail: readError(error)
    };
    renderUpdateStatus();
    showToast(readError(error), true);
  }
}

async function initializePortableUpdates() {
  if (!invoke || !elements.automaticUpdates) return;
  elements.automaticUpdates.checked = loadAutomaticUpdates();
  try {
    runtime.updateStatus = await invoke("get_portable_update_status");
    renderUpdateStatus();
    if (runtime.updateStatus.configured && elements.automaticUpdates.checked) {
      window.setTimeout(() => checkForPortableUpdate(true), 2500);
    }
    window.clearInterval(runtime.updateTimer);
    runtime.updateTimer = window.setInterval(() => {
      if (elements.automaticUpdates.checked && !document.hidden) {
        checkForPortableUpdate(true);
      }
    }, UPDATE_CHECK_INTERVAL_MS);
  } catch (error) {
    runtime.updateStatus = {
      configured: false,
      currentVersion: "",
      availableVersion: null,
      notes: "",
      phase: "error",
      detail: readError(error),
      downloaded: false
    };
    renderUpdateStatus();
  }
}

function loadDemoSettings() {
  try {
    const stored = JSON.parse(window.localStorage.getItem(DEMO_STORAGE_KEY) || "null");
    return sanitizeDemoSettings(stored);
  } catch {
    return { ...DEMO_DEFAULTS };
  }
}

function sanitizeDemoSettings(value) {
  const settings = { ...DEMO_DEFAULTS };
  if (!value || typeof value !== "object" || Array.isArray(value)) return settings;

  if (["top", "bottom"].includes(value.notificationPosition)) {
    settings.notificationPosition = value.notificationPosition;
  }
  if (["compact", "comfortable"].includes(value.notificationDensity)) {
    settings.notificationDensity = value.notificationDensity;
  }
  if (["compact", "expanded"].includes(value.audioLayout)) {
    settings.audioLayout = value.audioLayout;
  }
  for (const key of ["notificationActions", "showOutputDevice", "showMediaControls", "muted"]) {
    if (typeof value[key] === "boolean") settings[key] = value[key];
  }
  if (Number.isFinite(value.notificationDuration)) {
    settings.notificationDuration = Math.round(Math.max(3, Math.min(15, value.notificationDuration)));
  }
  if (Number.isFinite(value.volume)) {
    settings.volume = Math.round(Math.max(0, Math.min(100, value.volume)));
  }
  return settings;
}

function saveDemoSettings() {
  try {
    window.localStorage.setItem(DEMO_STORAGE_KEY, JSON.stringify(runtime.demo));
  } catch {
    // Storage can be unavailable in hardened WebView profiles; the live demo
    // remains functional for the current application session.
  }
}

function updateDemoSetting(key, value) {
  const candidate = sanitizeDemoSettings({ ...runtime.demo, [key]: value });
  runtime.demo = candidate;
  saveDemoSettings();
  renderDemoSettings();
}

function resetDemoSettings() {
  window.clearTimeout(runtime.demoNotificationTimer);
  runtime.demoNotificationTimer = 0;
  runtime.demo = { ...DEMO_DEFAULTS };
  runtime.demoMediaPlaying = false;
  saveDemoSettings();
  elements.notificationPreview?.classList.remove("is-hidden");
  elements.audioPreview?.classList.remove("is-hidden");
  renderDemoSettings();
  replayDemoSurface(elements.notificationPreview);
  replayDemoSurface(elements.audioPreview);
}

function renderDemoSettings() {
  const settings = runtime.demo;
  document.querySelectorAll("[data-demo-choice]").forEach((button) => {
    const selected = settings[button.dataset.demoChoice] === button.dataset.demoValue;
    button.classList.toggle("is-active", selected);
    button.setAttribute("aria-pressed", String(selected));
  });
  document.querySelectorAll("[data-demo-toggle]").forEach((control) => {
    control.checked = Boolean(settings[control.dataset.demoToggle]);
  });

  const duration = document.querySelector('[data-demo-range="notificationDuration"]');
  if (duration) {
    duration.value = String(settings.notificationDuration);
    setDemoRangeProgress(duration, settings.notificationDuration);
    const output = document.querySelector(`output[for="${duration.id}"]`);
    if (output) output.textContent = `${settings.notificationDuration} sec`;
  }

  document.querySelectorAll("[data-demo-volume]").forEach((control) => {
    control.value = String(settings.volume);
    setDemoRangeProgress(control, settings.volume);
  });
  document.querySelectorAll('[data-demo-volume-output], output[for="demo-volume"]').forEach((output) => {
    output.textContent = settings.muted ? "0" : `${settings.volume}${output.hasAttribute("data-demo-volume-output") ? "" : "%"}`;
  });
  document.querySelectorAll("[data-demo-volume-mute]").forEach((button) => {
    button.classList.toggle("is-muted", settings.muted);
    button.setAttribute("aria-pressed", String(settings.muted));
    button.setAttribute("aria-label", settings.muted ? "Unmute preview volume" : "Mute preview volume");
  });

  elements.demoDesktop?.classList.toggle("notification-bottom", settings.notificationPosition === "bottom");
  elements.notificationPreview?.classList.toggle("is-compact", settings.notificationDensity === "compact");
  elements.audioPreview?.classList.toggle("is-compact", settings.audioLayout === "compact");

  const actions = document.querySelector("[data-notification-actions]");
  if (actions) actions.hidden = !settings.notificationActions;
  const durationLabel = document.querySelector("[data-notification-duration-label]");
  if (durationLabel) durationLabel.textContent = `${settings.notificationDuration} second preview`;
  const device = document.querySelector("[data-audio-device]");
  if (device) device.hidden = !settings.showOutputDevice;
  const media = document.querySelector("[data-audio-media]");
  if (media) media.hidden = settings.audioLayout !== "expanded" || !settings.showMediaControls;
  renderDemoMediaState();
}

function setDemoRangeProgress(control, value) {
  const minimum = Number(control.min);
  const maximum = Number(control.max);
  const progress = maximum > minimum ? ((value - minimum) / (maximum - minimum)) * 100 : 0;
  control.style.setProperty("--range-progress", `${Math.max(0, Math.min(100, progress))}%`);
}

function replayDemoSurface(element) {
  if (!element) return;
  element.classList.remove("is-hidden", "is-replaying");
  void element.offsetWidth;
  element.classList.add("is-replaying");
  window.setTimeout(() => element.classList.remove("is-replaying"), 260);
}

function showDemoNotification() {
  window.clearTimeout(runtime.demoNotificationTimer);
  replayDemoSurface(elements.notificationPreview);
  runtime.demoNotificationTimer = window.setTimeout(
    dismissDemoNotification,
    runtime.demo.notificationDuration * 1000
  );
}

function dismissDemoNotification() {
  window.clearTimeout(runtime.demoNotificationTimer);
  runtime.demoNotificationTimer = 0;
  elements.notificationPreview?.classList.add("is-hidden");
}

function showDemoAudio() {
  replayDemoSurface(elements.audioPreview);
}

function toggleDemoMedia() {
  runtime.demoMediaPlaying = !runtime.demoMediaPlaying;
  renderDemoMediaState();
}

function renderDemoMediaState() {
  const button = document.querySelector("[data-demo-media-toggle]");
  if (!button) return;
  button.classList.toggle("is-playing", runtime.demoMediaPlaying);
  button.setAttribute("aria-label", runtime.demoMediaPlaying ? "Pause demo media" : "Play demo media");
  button.querySelector("use")?.setAttribute("href", runtime.demoMediaPlaying ? "#i-pause" : "#i-play");
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
    startThreePanelLayoutEnabled: settings.startMenuThreePanelLayoutEnabled,
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
    const threePanel = Boolean(settings?.startMenuThreePanelLayoutEnabled);
    const available = threePanel
      ? connected && settings?.startMenuHideAllAppsEditable
      : settings?.startMenuHideAllAppsSupported && settings?.startMenuHideAllAppsEditable;
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
  renderDemoSettings();
  updateControlAvailability();
  if (!invoke) {
    applyConnection(false, "Tauri IPC is unavailable");
    elements.activeCount.textContent = "Tauri IPC unavailable";
    elements.overviewSummary.textContent = "Run the bundled desktop application to connect to the native host.";
    return;
  }
  refreshState(true);
  schedulePolling();
  initializePortableUpdates();
}

start();
