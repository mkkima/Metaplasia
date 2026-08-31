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
  updates: "Updates",
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

const AGENT_RESULT_LABELS = {
  1: "Invalid configuration",
  2: "Incompatible process",
  3: "Initialization failed",
  4: "Native hook failed",
  5: "Agent not initialized",
  6: "Agent busy",
  7: "XAML adapter unavailable"
};

const HRESULT_LABELS = {
  "80004005": "Unspecified native failure (E_FAIL)",
  "80070005": "Access denied",
  "8007000D": "Invalid native data or visual-tree contract",
  "800705B4": "Operation timed out",
  "800401F0": "COM was not initialized on the calling thread",
  "8001010E": "COM interface was used from the wrong thread"
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
  startupEnabled: document.querySelector("#startup-enabled"),
  startupDetail: document.querySelector("#startup-detail"),
  refreshDiagnostics: document.querySelector("#refresh-diagnostics"),
  xamlSummary: document.querySelector("#xaml-summary"),
  xamlSectionTitle: document.querySelector("#xaml-section-title"),
  xamlTypes: document.querySelector("#xaml-types"),
  diagnosticLogSummary: document.querySelector("#diagnostic-log-summary"),
  diagnosticLogList: document.querySelector("#diagnostic-log-list"),
  diagnosticLogDirectory: document.querySelector("#diagnostic-log-directory"),
  diagnosticLogCount: document.querySelector("#diagnostic-log-count"),
  diagnosticLogLevel: document.querySelector("#diagnostic-log-level"),
  diagnosticLogTarget: document.querySelector("#diagnostic-log-target"),
  toastRegion: document.querySelector("#toast-region"),
  demoDesktop: document.querySelector("[data-demo-desktop]"),
  notificationPreview: document.querySelector("[data-notification-preview]"),
  audioPreview: document.querySelector("[data-audio-preview]"),
  applicationVersion: document.querySelector("#application-version"),
  updateCurrentVersion: document.querySelector("#update-current-version"),
  updateCurrentChannel: document.querySelector("#update-current-channel"),
  updateCacheSize: document.querySelector("#update-cache-size"),
  updateStoragePath: document.querySelector("#update-storage-path"),
  updateChannelLabel: document.querySelector("#update-channel-label"),
  automaticUpdates: document.querySelector("#automatic-updates"),
  checkUpdates: document.querySelector("#check-updates"),
  downloadUpdate: document.querySelector("#download-update"),
  installUpdate: document.querySelector("#install-update"),
  updateStatusBar: document.querySelector("#update-status-bar"),
  updateStatusTitle: document.querySelector("#update-status-title"),
  updateStatusDetail: document.querySelector("#update-status-detail"),
  updateNotes: document.querySelector("#update-notes"),
  updateSkip: document.querySelector("#update-skip"),
  updateSkipTitle: document.querySelector("#update-skip-title"),
  resumeSkippedUpdate: document.querySelector("#resume-skipped-update"),
  refreshUpdateHistory: document.querySelector("#refresh-update-history"),
  clearUpdateCache: document.querySelector("#clear-update-cache"),
  updateHistoryList: document.querySelector("#update-history-list"),
  versionHistorySummary: document.querySelector("#version-history-summary"),
  rollbackList: document.querySelector("#rollback-list"),
  rollbackSummary: document.querySelector("#rollback-summary"),
  updateConfirmDialog: document.querySelector("#update-confirm-dialog"),
  rollbackConfirmDialog: document.querySelector("#rollback-confirm-dialog"),
  rollbackConfirmDetail: document.querySelector("#rollback-confirm-detail")
};

const runtime = {
  route: "overview",
  state: null,
  refreshing: false,
  startupBusy: false,
  startupStatus: null,
  pending: new Set(),
  pollTimer: 0,
  demo: loadDemoSettings(),
  demoNotificationTimer: 0,
  demoMediaPlaying: false,
  updateStatus: null,
  updateBusy: false,
  updateTimer: 0,
  updateHistory: null,
  updateHistoryBusy: false,
  rollbackVersion: null,
  xamlTarget: "taskbar",
  diagnosticLogs: [],
  diagnosticLogFingerprint: "",
  diagnosticLogsRefreshing: false,
  diagnosticPollCount: 0
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
      </dl>
      <div class="diagnostic-analysis" data-diagnostic="analysis">
        <span>Host detail</span><strong data-diagnostic="summary">Waiting for a snapshot</strong>
        <p data-diagnostic="explanation" hidden></p>
        <dl class="diagnostic-native-facts" data-diagnostic="native-facts" hidden>
          <dt>Agent result</dt><dd data-diagnostic="result">—</dd>
          <dt>Native HRESULT</dt><dd data-diagnostic="native">—</dd>
          <dt>Failure stage</dt><dd data-diagnostic="stage">—</dd>
          <dt>Controller</dt><dd data-diagnostic="controller">—</dd>
        </dl>
        <code data-diagnostic="detail">Waiting</code>
      </div>
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
  if (route === "diagnostics") {
    updateDiagnosticCards();
    refreshDiagnosticLogs(true);
  } else if (route === "updates") {
    if (runtime.updateHistory) renderUpdateHistory();
    else refreshUpdateHistory(true);
  }
}

function bindNavigation() {
  document.addEventListener("click", (event) => {
    const routeButton = event.target.closest("[data-route]");
    const targetButton = event.target.closest("[data-open-target]");
    const rollbackButton = event.target.closest("[data-rollback-version]");
    if (routeButton) navigate(routeButton.dataset.route);
    if (targetButton) navigate(targetButton.dataset.openTarget);
    if (rollbackButton) confirmRollback(rollbackButton.dataset.rollbackVersion);
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

  elements.refreshDiagnostics.addEventListener("click", refreshDiagnostics);
  elements.startupEnabled?.addEventListener("change", setStartupEnabled);
  document.querySelectorAll("[data-xaml-target]").forEach((button) => {
    button.addEventListener("click", () => {
      runtime.xamlTarget = button.dataset.xamlTarget;
      document.querySelectorAll("[data-xaml-target]").forEach((candidate) => {
        const selected = candidate === button;
        candidate.classList.toggle("is-active", selected);
        candidate.setAttribute("aria-pressed", String(selected));
      });
      refreshXamlDiagnostics();
    });
  });
  elements.diagnosticLogLevel?.addEventListener("change", renderDiagnosticLogs);
  elements.diagnosticLogTarget?.addEventListener("change", renderDiagnosticLogs);

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
  elements.refreshUpdateHistory?.addEventListener("click", () => refreshUpdateHistory(false));
  elements.clearUpdateCache?.addEventListener("click", clearPortableUpdateCache);
  elements.resumeSkippedUpdate?.addEventListener("click", resumeSkippedUpdate);
  elements.rollbackConfirmDialog?.addEventListener("close", () => {
    if (elements.rollbackConfirmDialog.returnValue === "rollback") {
      performRollback(runtime.rollbackVersion);
    } else {
      runtime.rollbackVersion = null;
    }
  });
}

function renderStartupStatus(status = runtime.startupStatus) {
  if (!status || !elements.startupEnabled || !elements.startupDetail) return;
  runtime.startupStatus = status;
  elements.startupEnabled.checked = Boolean(status.enabled);
  elements.startupEnabled.disabled = runtime.startupBusy;
  elements.startupDetail.textContent = status.detail || (status.enabled
    ? "Starts in the system tray at sign-in."
    : "Autostart is disabled.");
}

async function refreshStartupStatus(silent = false) {
  if (!invoke || runtime.startupBusy) return;
  runtime.startupBusy = true;
  if (elements.startupEnabled) elements.startupEnabled.disabled = true;
  try {
    runtime.startupStatus = await invoke("get_startup_status");
    renderStartupStatus();
  } catch (error) {
    if (elements.startupDetail) elements.startupDetail.textContent = readError(error);
    if (!silent) showToast(readError(error), true);
  } finally {
    runtime.startupBusy = false;
    if (elements.startupEnabled) elements.startupEnabled.disabled = false;
  }
}

async function setStartupEnabled() {
  if (!invoke || runtime.startupBusy || !elements.startupEnabled) return;
  const enabled = elements.startupEnabled.checked;
  runtime.startupBusy = true;
  elements.startupEnabled.disabled = true;
  try {
    runtime.startupStatus = await invoke("set_startup_enabled", { enabled });
    renderStartupStatus();
    showToast(enabled
      ? "Metaplasia will start in the system tray with Windows."
      : "Metaplasia autostart is disabled.", false);
  } catch (error) {
    elements.startupEnabled.checked = Boolean(runtime.startupStatus?.enabled);
    if (elements.startupDetail) elements.startupDetail.textContent = readError(error);
    showToast(readError(error), true);
  } finally {
    runtime.startupBusy = false;
    elements.startupEnabled.disabled = false;
  }
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
    // setting still works; the switch remains visible before any automatic
    // update can be applied.
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
    case "rollback-ready": return version ? `Rollback ${version} is ready` : "Rollback is ready";
    case "installing": return status?.operation === "rollback"
      ? "Installing the verified rollback…"
      : "Installing the verified portable update…";
    case "error": return "Portable update failed";
    default: return "Portable update channel";
  }
}

function renderUpdateStatus(status = runtime.updateStatus) {
  if (!status || !elements.updateStatusTitle) return;
  runtime.updateStatus = status;
  elements.applicationVersion.textContent = `Metaplasia ${status.currentVersion}`;
  if (elements.updateCurrentVersion) {
    elements.updateCurrentVersion.textContent = `Metaplasia ${status.currentVersion}`;
  }
  if (elements.updateCurrentChannel) {
    elements.updateCurrentChannel.textContent = status.channel === "development"
      ? "Development portable channel"
      : "Stable portable channel";
  }
  if (elements.updateChannelLabel) {
    elements.updateChannelLabel.textContent = status.channel === "development"
      ? "Development portable channel"
      : "Stable portable channel";
  }
  elements.updateStatusTitle.textContent = updatePhaseTitle(status);
  elements.updateStatusDetail.textContent = status.detail || "Ready to check GitHub Releases.";
  elements.updateNotes.textContent = status.notes || "";
  elements.updateNotes.hidden = !status.notes;
  if (elements.updateSkip) {
    elements.updateSkip.hidden = !status.skippedVersion;
    elements.updateSkipTitle.textContent = status.skippedVersion
      ? `Version ${status.skippedVersion} is skipped`
      : "A version is skipped";
  }
  const isError = status.phase === "error";
  const isPositive = ["current", "available", "ready", "rollback-ready"].includes(status.phase);
  elements.updateStatusBar.classList.toggle("is-error", isError);
  elements.updateStatusBar.classList.toggle("is-active", isPositive);

  const enabled = Boolean(status.configured) && !runtime.updateBusy;
  elements.checkUpdates.disabled = !enabled;
  elements.downloadUpdate.hidden = status.phase !== "available" || status.downloaded;
  elements.downloadUpdate.disabled = !enabled;
  elements.installUpdate.hidden = status.phase !== "ready" || !status.downloaded;
  elements.installUpdate.disabled = !enabled;
  elements.automaticUpdates.disabled = !status.configured || runtime.updateBusy;
  if (elements.clearUpdateCache) elements.clearUpdateCache.disabled = runtime.updateBusy;
  if (elements.resumeSkippedUpdate) elements.resumeSkippedUpdate.disabled = runtime.updateBusy;
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
    if (automatic && status.phase === "ready" && status.downloaded) {
      runtime.updateStatus = {
        ...status,
        phase: "installing",
        detail: "The signed portable update is verified. Restarting Metaplasia and Windows shell components."
      };
      renderUpdateStatus();
      await invoke("apply_portable_update");
    }
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
    renderUpdateHistory();
    if (runtime.route === "updates") refreshUpdateHistory(true);
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
    renderUpdateHistory();
    if (runtime.route === "updates") refreshUpdateHistory(true);
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

function formatUpdateBytes(value) {
  const bytes = Number(value);
  if (!Number.isFinite(bytes) || bytes <= 0) return "Empty";
  const units = ["B", "KB", "MB", "GB"];
  let amount = bytes;
  let unit = 0;
  while (amount >= 1024 && unit < units.length - 1) {
    amount /= 1024;
    unit += 1;
  }
  return `${amount >= 10 || unit === 0 ? amount.toFixed(0) : amount.toFixed(1)} ${units[unit]}`;
}

function releaseRelationLabel(relation) {
  if (relation === "current") return "Current";
  if (relation === "available") return "Update available";
  return "Previous";
}

function renderUpdateHistory(snapshot = runtime.updateHistory) {
  if (!snapshot) return;
  runtime.updateHistory = snapshot;
  if (elements.updateCurrentVersion && snapshot.currentVersion) {
    elements.updateCurrentVersion.textContent = `Metaplasia ${snapshot.currentVersion}`;
  }
  if (elements.updateCurrentChannel && snapshot.channel) {
    elements.updateCurrentChannel.textContent = snapshot.channel === "development"
      ? "Development portable channel"
      : "Stable portable channel";
  }
  if (runtime.updateStatus && runtime.updateStatus.skippedVersion !== snapshot.skippedVersion) {
    runtime.updateStatus = { ...runtime.updateStatus, skippedVersion: snapshot.skippedVersion || null };
    renderUpdateStatus();
  }
  if (elements.updateCacheSize) elements.updateCacheSize.textContent = formatUpdateBytes(snapshot.cacheBytes);
  if (elements.updateStoragePath) {
    elements.updateStoragePath.textContent = snapshot.storagePath || "%LOCALAPPDATA%\\Metaplasia\\updates";
    elements.updateStoragePath.title = elements.updateStoragePath.textContent;
  }
  if (elements.versionHistorySummary) {
    elements.versionHistorySummary.textContent = snapshot.versionCatalogDetail || "Published version catalog loaded.";
  }
  if (elements.rollbackSummary) {
    elements.rollbackSummary.textContent = snapshot.rollbackCatalogDetail || "Signed rollback catalog loaded.";
  }

  if (elements.updateHistoryList) {
    elements.updateHistoryList.replaceChildren();
    if (!snapshot.versions?.length) {
      const empty = document.createElement("p");
      empty.className = "update-empty";
      empty.textContent = snapshot.versionCatalogDetail || "No published portable versions are available.";
      elements.updateHistoryList.append(empty);
    } else {
      snapshot.versions.forEach((release) => {
        const article = document.createElement("article");
        const relation = ["available", "current", "previous"].includes(release.relation)
          ? release.relation
          : "previous";
        article.className = `update-history-entry is-${relation}`;
        const marker = document.createElement("span");
        marker.className = "update-history-marker";
        const copy = document.createElement("div");
        copy.className = "update-history-copy";
        const title = document.createElement("strong");
        title.textContent = `Metaplasia ${release.version}`;
        const detail = document.createElement("p");
        const published = Date.parse(release.publishedAt || "");
        detail.textContent = Number.isFinite(published)
          ? `Published ${new Intl.DateTimeFormat(undefined, { dateStyle: "medium" }).format(new Date(published))}`
          : "Published portable release";
        copy.append(title, detail);
        const meta = document.createElement("div");
        meta.className = "update-history-meta";
        const badge = document.createElement("span");
        badge.className = "update-history-badge";
        badge.textContent = releaseRelationLabel(relation);
        meta.append(badge);
        article.append(marker, copy, meta);
        elements.updateHistoryList.append(article);
      });
    }
  }

  if (elements.rollbackList) {
    elements.rollbackList.replaceChildren();
    if (!snapshot.availableRollbacks?.length) {
      const empty = document.createElement("p");
      empty.className = "update-empty";
      empty.textContent = snapshot.rollbackCatalogDetail || "No older signed releases are available.";
      elements.rollbackList.append(empty);
    } else {
      snapshot.availableRollbacks.forEach((release) => {
        const article = document.createElement("article");
        article.className = "rollback-entry";
        const marker = document.createElement("span");
        marker.className = "update-history-marker";
        const copy = document.createElement("div");
        copy.className = "rollback-copy";
        const title = document.createElement("strong");
        title.textContent = `Metaplasia ${release.version}`;
        const detail = document.createElement("p");
        const published = Date.parse(release.publishedAt || "");
        detail.textContent = Number.isFinite(published)
          ? `Published ${new Intl.DateTimeFormat(undefined, { dateStyle: "medium" }).format(new Date(published))}`
          : "Signed portable release";
        copy.append(title, detail);
        const button = document.createElement("button");
        button.className = "secondary-button";
        button.type = "button";
        button.dataset.rollbackVersion = release.version;
        button.disabled = runtime.updateBusy;
        button.textContent = "Rollback";
        article.append(marker, copy, button);
        elements.rollbackList.append(article);
      });
    }
  }
}

async function refreshUpdateHistory(silent = false) {
  if (!invoke || runtime.updateHistoryBusy) return;
  runtime.updateHistoryBusy = true;
  if (elements.refreshUpdateHistory) elements.refreshUpdateHistory.disabled = true;
  try {
    runtime.updateHistory = await invoke("get_portable_update_history");
    renderUpdateHistory();
  } catch (error) {
    if (!silent) showToast(readError(error), true);
    if (elements.versionHistorySummary) elements.versionHistorySummary.textContent = readError(error);
    if (elements.rollbackSummary) elements.rollbackSummary.textContent = readError(error);
  } finally {
    runtime.updateHistoryBusy = false;
    if (elements.refreshUpdateHistory) elements.refreshUpdateHistory.disabled = false;
  }
}

function confirmRollback(version) {
  if (runtime.updateBusy || !/^\d+\.\d+\.\d+$/.test(version || "")) return;
  const allowed = runtime.updateHistory?.availableRollbacks?.some((release) => release.version === version);
  if (!allowed) {
    showToast("Refresh the signed rollback catalog before selecting a version.", true);
    return;
  }
  runtime.rollbackVersion = version;
  if (elements.rollbackConfirmDetail) {
    elements.rollbackConfirmDetail.textContent = `Metaplasia ${version} will replace the current ${runtime.updateStatus?.currentVersion || "portable"} binary set after its signature and package hash are verified.`;
  }
  elements.rollbackConfirmDialog?.showModal();
}

async function performRollback(version) {
  if (!invoke || runtime.updateBusy || !/^\d+\.\d+\.\d+$/.test(version || "")) return;
  const automaticWasEnabled = elements.automaticUpdates.checked;
  elements.automaticUpdates.checked = false;
  saveAutomaticUpdates(false);
  runtime.updateBusy = true;
  runtime.updateStatus = {
    ...(runtime.updateStatus || {}),
    availableVersion: version,
    operation: "rollback",
    phase: "downloading",
    detail: `Downloading and verifying signed rollback ${version}.`,
    downloaded: false
  };
  renderUpdateStatus();
  renderUpdateHistory();
  try {
    runtime.updateStatus = await invoke("prepare_portable_rollback", { version });
    runtime.updateStatus = {
      ...runtime.updateStatus,
      phase: "installing",
      detail: `Installing rollback ${version} and restarting Windows shell components.`
    };
    renderUpdateStatus();
    await invoke("apply_portable_update");
  } catch (error) {
    elements.automaticUpdates.checked = automaticWasEnabled;
    saveAutomaticUpdates(automaticWasEnabled);
    runtime.updateBusy = false;
    runtime.updateStatus = {
      ...(runtime.updateStatus || {}),
      phase: "error",
      detail: readError(error),
      downloaded: false
    };
    renderUpdateStatus();
    renderUpdateHistory();
    showToast(readError(error), true);
  } finally {
    runtime.rollbackVersion = null;
  }
}

async function clearPortableUpdateCache() {
  if (!invoke || runtime.updateBusy) return;
  runtime.updateBusy = true;
  renderUpdateStatus();
  try {
    await invoke("clear_portable_update_cache");
    runtime.updateStatus = await invoke("get_portable_update_status");
    await refreshUpdateHistory(true);
    showToast("Downloaded update files were cleared.", false);
  } catch (error) {
    showToast(readError(error), true);
  } finally {
    runtime.updateBusy = false;
    renderUpdateStatus();
    renderUpdateHistory();
  }
}

async function resumeSkippedUpdate() {
  if (!invoke || runtime.updateBusy) return;
  runtime.updateBusy = true;
  renderUpdateStatus();
  try {
    await invoke("resume_portable_update_version");
    runtime.updateStatus = await invoke("get_portable_update_status");
    await refreshUpdateHistory(true);
    showToast("The skipped version is allowed again.", false);
  } catch (error) {
    showToast(readError(error), true);
  } finally {
    runtime.updateBusy = false;
    renderUpdateStatus();
  }
  if (elements.automaticUpdates.checked) checkForPortableUpdate(true);
}

async function initializePortableUpdates() {
  if (!invoke || !elements.automaticUpdates) return;
  elements.automaticUpdates.checked = loadAutomaticUpdates();
  try {
    runtime.updateStatus = await invoke("get_portable_update_status");
    renderUpdateStatus();
    refreshUpdateHistory(true);
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
    const interpretation = interpretTargetDiagnostic(target);
    const analysis = card.querySelector('[data-diagnostic="analysis"]');
    const explanation = card.querySelector('[data-diagnostic="explanation"]');
    const facts = card.querySelector('[data-diagnostic="native-facts"]');
    card.querySelector('[data-diagnostic="summary"]').textContent = interpretation.summary;
    card.querySelector('[data-diagnostic="detail"]').textContent = target.detail || "No raw host detail";
    explanation.textContent = interpretation.explanation;
    explanation.hidden = !interpretation.explanation;
    facts.hidden = !interpretation.hasNativeFacts;
    card.querySelector('[data-diagnostic="result"]').textContent = interpretation.result;
    card.querySelector('[data-diagnostic="native"]').textContent = interpretation.native;
    card.querySelector('[data-diagnostic="stage"]').textContent = interpretation.stage;
    card.querySelector('[data-diagnostic="controller"]').textContent = interpretation.controller;
    analysis.classList.toggle("is-error", target.state === "error" || target.state === "incompatible");
  }
}

function interpretTargetDiagnostic(target) {
  const detail = target.detail || "No detail was returned by the native host.";
  const resultMatch = detail.match(/code\s+(\d+)/i);
  const nativeMatch = detail.match(/native=0x([0-9a-f]+)/i);
  const stageMatch = detail.match(/stage=([^,)\s]+)/i);
  const stateMatch = detail.match(/state=0x([0-9a-f]+)/i);
  const resultCode = resultMatch ? Number(resultMatch[1]) : null;
  const nativeCode = nativeMatch?.[1]?.toUpperCase() || "";
  const stage = stageMatch?.[1] || "";
  const controllerState = stateMatch ? Number.parseInt(stateMatch[1], 16) : null;
  const hasNativeFacts = resultCode !== null || Boolean(nativeCode || stage) || controllerState !== null;

  let summary = detail;
  let explanation = "";
  if (target.state === "error" && resultCode === 7 && stage === "advise-visual-tree") {
    summary = "Windows XAML visual-tree subscription failed";
    explanation = "The agent and XAML controller loaded, but the callback subscription that observes this Windows shell surface was not established. The incomplete style change was rolled back.";
  } else if (target.state === "error" && nativeCode === "8007000D" && stage === "create-frame-envelope") {
    summary = "Start frame visual-tree contract mismatch";
    explanation = "The agent loaded, but the visual parent chain around the Start frame did not match the approved Windows shell structure. No incomplete three-panel scene was committed.";
  } else if (target.state === "error" && resultCode !== null) {
    summary = AGENT_RESULT_LABELS[resultCode] || `Native agent failure ${resultCode}`;
    explanation = "The native agent rejected the requested configuration and rolled back the incomplete change.";
  } else if (target.state === "incompatible") {
    summary = "This Windows shell build is not approved";
    explanation = "Metaplasia failed closed because the loaded Windows binaries do not match a certified compatibility profile.";
  } else if (target.state === "active") {
    summary = "Target is active and responding";
  } else if (target.state === "disabled") {
    summary = "Target is disabled";
  }

  const result = resultCode === null
    ? "—"
    : `${AGENT_RESULT_LABELS[resultCode] || "Unknown result"} (${resultCode})`;
  const native = nativeCode
    ? `0x${nativeCode} · ${HRESULT_LABELS[nativeCode] || "Unmapped HRESULT"}`
    : resultCode !== null ? "Not reported" : "—";
  return {
    summary,
    explanation,
    hasNativeFacts,
    result,
    native,
    stage: stage || "—",
    controller: controllerState === null ? "—" : describeControllerState(controllerState)
  };
}

function describeControllerState(state) {
  const flags = [];
  if (state & 0x1) flags.push("service");
  if (state & 0x2) flags.push("watcher");
  if (state & 0x4) flags.push("subscribed");
  const tracked = state >>> 8;
  flags.push(`${tracked} tracked`);
  return `0x${state.toString(16).toUpperCase()} · ${flags.join(" · ")}`;
}

async function refreshDiagnostics() {
  if (elements.refreshDiagnostics.disabled) return;
  elements.refreshDiagnostics.disabled = true;
  try {
    await Promise.allSettled([
      refreshState(true),
      refreshXamlDiagnostics(true),
      refreshDiagnosticLogs(false)
    ]);
  } finally {
    elements.refreshDiagnostics.disabled = false;
  }
}

async function refreshXamlDiagnostics(silent = false) {
  if (!invoke || !runtime.state?.connected) {
    if (!silent) showToast("The native host is not connected.", true);
    return;
  }
  const label = runtime.xamlTarget === "taskbar" ? "Taskbar" : "Start menu";
  elements.xamlSectionTitle.textContent = `${label} XAML observations`;
  try {
    const diagnostics = await invoke("get_xaml_diagnostics", { target: runtime.xamlTarget });
    const health = runtime.xamlTarget === "start"
      ? ` · style ${diagnostics.styleState} · stage ${diagnostics.styleStage} · dependencies 0x${diagnostics.sceneDependencies.toString(16).padStart(2, "0").toUpperCase()} · attempts ${diagnostics.styleApplyAttemptCount} · successes ${diagnostics.styleApplySuccessCount} · failures ${diagnostics.styleApplyFailureCount}${diagnostics.lastStyleError ? ` · HRESULT 0x${diagnostics.lastStyleError.toString(16).padStart(8, "0").toUpperCase()}` : ""}`
      : "";
    elements.xamlSummary.textContent = `${diagnostics.trackedElementCount} tracked${health} · ${diagnostics.droppedTypeCount} dropped types · ${diagnostics.droppedElementCount} dropped elements`;
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
    elements.xamlSummary.textContent = `${label} diagnostics failed: ${readError(error)}`;
    elements.xamlTypes.replaceChildren();
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 2;
    cell.className = "empty-cell";
    cell.textContent = `Unable to load ${label} observations.`;
    row.append(cell);
    elements.xamlTypes.append(row);
    if (!silent) showToast(readError(error), true);
  }
}

async function refreshDiagnosticLogs(silent = false) {
  if (!invoke || runtime.diagnosticLogsRefreshing) return;
  runtime.diagnosticLogsRefreshing = true;
  try {
    const batch = await invoke("get_diagnostic_logs");
    const entries = Array.isArray(batch.entries) ? batch.entries : [];
    const first = entries[0];
    const last = entries.at(-1);
    const fingerprint = [
      entries.length,
      first?.timestamp, first?.event, first?.message,
      last?.timestamp, last?.event, last?.message
    ].join("\u001f");
    const logsChanged = fingerprint !== runtime.diagnosticLogFingerprint;
    runtime.diagnosticLogs = entries;
    runtime.diagnosticLogFingerprint = fingerprint;
    elements.diagnosticLogDirectory.textContent = batch.directory || "%LOCALAPPDATA%\\Metaplasia\\logs";
    const qualifiers = [];
    if (batch.truncated) qualifiers.push("showing the newest bounded entries");
    if (batch.invalidLineCount) qualifiers.push(`${batch.invalidLineCount} malformed lines ignored`);
    if (batch.unreadableFileCount) qualifiers.push(`${batch.unreadableFileCount} log files unavailable`);
    elements.diagnosticLogSummary.textContent = qualifiers.length
      ? `Local rotating log · ${qualifiers.join(" · ")}`
      : "Local rotating log · 1 MB current file and 3 backups maximum.";
    if (logsChanged) renderDiagnosticLogs();
  } catch (error) {
    elements.diagnosticLogSummary.textContent = `Unable to read local logs: ${readError(error)}`;
    if (!silent) showToast(readError(error), true);
  } finally {
    runtime.diagnosticLogsRefreshing = false;
  }
}

function renderDiagnosticLogs() {
  const level = elements.diagnosticLogLevel?.value || "all";
  const target = elements.diagnosticLogTarget?.value || "all";
  const entries = runtime.diagnosticLogs.filter((entry) => {
    const levelMatches = level === "all" || entry.level === level;
    const targets = String(entry.target || "").split("+");
    const sharedExplorerGroup = entry.target === "explorer-shell"
      && (target === "taskbar" || target === "file-explorer");
    const targetMatches = target === "all" || targets.includes(target) || sharedExplorerGroup;
    return levelMatches && targetMatches;
  }).reverse();

  elements.diagnosticLogList.replaceChildren();
  if (!entries.length) {
    const empty = document.createElement("p");
    empty.className = "diagnostic-log-empty";
    empty.textContent = runtime.diagnosticLogs.length
      ? "No entries match the selected filters."
      : "No host log entries exist yet. Restart the rebuilt host to begin logging.";
    elements.diagnosticLogList.append(empty);
  } else {
    entries.forEach((entry) => elements.diagnosticLogList.append(buildDiagnosticLogEntry(entry)));
  }
  elements.diagnosticLogCount.textContent = `${entries.length} of ${runtime.diagnosticLogs.length} entries`;
}

function buildDiagnosticLogEntry(entry) {
  const row = document.createElement("article");
  row.className = `diagnostic-log-entry is-${entry.level}`;
  const marker = document.createElement("span");
  marker.className = "diagnostic-log-marker";
  marker.setAttribute("aria-hidden", "true");
  const body = document.createElement("div");
  const header = document.createElement("div");
  header.className = "diagnostic-log-entry-header";
  const time = document.createElement("time");
  time.dateTime = entry.timestamp;
  time.textContent = formatLogTimestamp(entry.timestamp);
  const badge = document.createElement("span");
  badge.className = "diagnostic-log-level";
  badge.textContent = entry.level;
  const context = document.createElement("code");
  context.textContent = `${entry.component}:${entry.event} · ${entry.target}${entry.processId ? ` · PID ${entry.processId}` : ""}`;
  header.append(time, badge, context);
  const message = document.createElement("p");
  message.textContent = entry.message;
  body.append(header, message);
  row.append(marker, body);
  return row;
}

function formatLogTimestamp(value) {
  const timestamp = new Date(value);
  if (Number.isNaN(timestamp.getTime())) return value;
  return timestamp.toLocaleString(undefined, {
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    fractionalSecondDigits: 3, hour12: false
  });
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
  runtime.pollTimer = window.setInterval(() => {
    refreshState(false);
    if (runtime.route === "diagnostics") {
      runtime.diagnosticPollCount += 1;
      if (runtime.diagnosticPollCount >= 4) {
        runtime.diagnosticPollCount = 0;
        refreshDiagnosticLogs(true);
      }
    } else {
      runtime.diagnosticPollCount = 0;
    }
  }, 1200);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) {
      refreshState(true);
      if (runtime.route === "diagnostics") refreshDiagnosticLogs(true);
    }
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
  refreshStartupStatus(true);
  schedulePolling();
  initializePortableUpdates();
}

start();
