"use strict";
// Page shell: details panel, API console, toasts and session activity.
// Presentation only. It reads state owned by app.js, never changes how sign-in,
// history or generation work, and keeps nothing in URLs or browser storage.

// Room flow: add a computer to this household with a short code, or move this
// computer into another household with one. It lives in the Computers panel
// and is offered only to a sign-in that may manage this computer. The code is
// kept in this page's memory and on screen, nowhere else.
const ROOM_FLOW_ENABLED = true;

(() => {
  const app = $("app"), details = $("details");
  const narrow = window.matchMedia("(max-width: 760px)");
  const panelTitles = {model: "Model", devices: "Computers", activity: "Activity"};
  let lastPanelButton = null, panel = "model";
  let endpointConfig = null, endpointPending = false;
  let devicesPending = false, devicesExpiry = 0;
  let wasConnected = false, wasBusy = false, lastNotice = "";

  const connected = () => app.classList.contains("connected");
  const element = (tag, className, text) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  };

  // ---- toasts and activity ----
  function toast(text) {
    const node = element("div", "toast", text);
    $("toasts").append(node);
    setTimeout(() => node.classList.add("leaving"), 3600);
    setTimeout(() => node.remove(), 4000);
  }
  function record(text) {
    const entry = element("div"), stamp = element("time", "", new Date().toLocaleTimeString());
    entry.append(stamp, document.createTextNode(text));
    $("activity-log").prepend(entry);
    while ($("activity-log").childElementCount > 60) $("activity-log").lastElementChild.remove();
  }

  // ---- details panel ----
  function openDetails(name, button) {
    if (!connected()) return;
    if (name === "devices" && !operator) name = "model";
    panel = name; lastPanelButton = button || lastPanelButton;
    if (narrow.matches) sidebar(false);
    details.hidden = false; details.inert = false; app.classList.add("details-open");
    for (const section of details.querySelectorAll("[data-panel]")) section.hidden = section.dataset.panel !== name;
    for (const control of document.querySelectorAll("[data-panel-open]"))
      control.setAttribute("aria-expanded", String(control.dataset.panelOpen === name));
    $("details-title").textContent = panelTitles[name];
    if (name === "model") renderModel();
    if (name === "devices") { loadDevices(); roomRefresh(); }
    $("details-close").focus();
  }
  function closeDetails(restoreFocus = false) {
    if (details.hidden) return;
    details.hidden = true; details.inert = true; app.classList.remove("details-open");
    for (const control of document.querySelectorAll("[data-panel-open]")) control.setAttribute("aria-expanded", "false");
    if (restoreFocus && lastPanelButton?.isConnected && !lastPanelButton.hidden) lastPanelButton.focus();
  }
  function closePanels() {
    closeDetails(true);
    if (narrow.matches && connected()) sidebar(false);
  }
  for (const control of document.querySelectorAll("[data-panel-open]"))
    control.addEventListener("click", () => {
      if (!details.hidden && panel === control.dataset.panelOpen && !details.contains(control)) closeDetails(true);
      else openDetails(control.dataset.panelOpen, details.contains(control) ? null : control);
    });
  $("details-close").addEventListener("click", () => closeDetails(true));
  $("panel-scrim").addEventListener("click", closePanels);
  document.addEventListener("keydown", event => {
    if (event.key === "Escape" && (!details.hidden || (narrow.matches && connected() && !app.classList.contains("sidebar-hidden")))) closePanels();
  });

  // ---- model facts ----
  function facts(parent, rows) {
    const list = element("dl");
    for (const [label, value, mono] of rows) {
      if (value === undefined || value === null || value === "") continue;
      list.append(element("dt", "", label), element("dd", mono ? "mono" : "", String(value)));
    }
    parent.append(list);
  }
  function renderModel() {
    const selected = models.find(model => model.id === $("model").value);
    $("model-facts").replaceChildren();
    if (!selected) $("model-facts").append(element("p", "panel-empty",
      models.length ? "The saved model of this conversation is currently unavailable." : "No approved resident models are assigned to this token yet."));
    else facts($("model-facts"), [
      ["Name", selected.name],
      ["Context", Number.isInteger(selected.context) ? `${selected.context} tokens` : null],
      ["Output limit", Number.isInteger(selected.max_tokens) ? `${selected.max_tokens} tokens per reply` : null],
      ["Sessions", Number.isInteger(selected.sessions) ? String(selected.sessions) : null],
      ["State", typeof selected.state === "string" ? selected.state.replaceAll("_", " ") : null],
      ["Model id", selected.id, true]]);
    $("model-list").replaceChildren();
    for (const model of models) {
      const card = element("div", "peer-card"), name = element("div", "peer-name");
      card.classList.toggle("self", model === selected);
      name.append(element("span", model === selected ? "dot ok" : "dot"), document.createTextNode(model.name));
      card.append(name); $("model-list").append(card);
    }
    $("endpoint-model").value = selected ? selected.id : "";
    renderExample();
  }

  // ---- computers (operator reports) ----
  function clearDevices(message = "") {
    devicesExpiry = 0;
    $("peers").replaceChildren(); $("cluster-summary").textContent = "—";
    $("devices-status").textContent = message; $("device-count").textContent = "Computers";
  }
  function renderDevices(data) {
    clearDevices();
    if (!data.inventory_ok) { $("devices-status").textContent = "Inventory unavailable — live resource values withheld."; return; }
    if (!data.nodes.length) { $("devices-status").textContent = "No current machine reports."; return; }
    let offered = 0;
    for (const node of data.nodes) {
      const card = element("div", "peer-card"), name = element("div", "peer-name"), stats = element("div", "peer-stats");
      const workload = node.workload;
      name.append(element("span", "dot ok"), document.createTextNode(node.name));
      const stat = (label, value) => { const item = element("span"); item.append(`${label} `, element("b", "", value)); stats.append(item); };
      stat("RAM free", gib(node.ram_available_bytes));
      stat("offered", gib(node.ram_offered_bytes));
      if (workload) stat("work", `${workload.active} active · ${workload.queued} queued`);
      card.append(name, element("div", "peer-gpu", `${node.cpu} · ${node.os} / ${node.arch}`), stats);
      card.dataset.computer = node.name;
      $("peers").append(card);
      if (Number.isFinite(node.ram_offered_bytes)) offered += node.ram_offered_bytes;
    }
    const count = `${data.nodes.length} ${data.nodes.length === 1 ? "computer" : "computers"}`;
    $("device-count").textContent = count;
    $("cluster-summary").textContent = `${count} · ${gib(offered)} offered`;
    $("devices-status").textContent = `Reports captured at ${new Date(data.captured_at * 1000).toLocaleTimeString()}.`;
    devicesExpiry = Date.now() + Math.max(0, data.inventory_ttl_ms - Math.max(...data.nodes.map(node => node.age_ms)));
    renderMemory();
  }
  // What this computer's sharing service reports about locking model memory,
  // shown next to this computer. Other computers report on their own pages.
  function renderMemory() {
    for (const old of $("peers").querySelectorAll(".peer-memory")) old.remove();
    if (!approvalMemory?.warning || !approvalMemory.detail || !$("peers").childElementCount) return;
    const own = Array.from($("peers").children).find(card => card.dataset.computer === approvalMemory.computer);
    const note = element("div", "peer-memory", own ? approvalMemory.detail : `This computer: ${approvalMemory.detail}`);
    if (own) own.append(note); else $("peers").prepend(note);
  }
  async function loadDevices() {
    if (!operator || !token || devicesPending) return;
    // A response in progress already holds one of the two per-user request
    // permits and needs the other to save its turn; do not compete with it.
    if (busy) { if (!$("peers").childElementCount) $("devices-status").textContent = "Reports are not read while a response is being generated."; return; }
    const credential = token;
    devicesPending = true; $("devices-refresh").disabled = true;
    if (!$("peers").childElementCount) $("devices-status").textContent = "Reading the local service…";
    try {
      const data = await request("/api/v1/workspace");
      if (credential === token) renderDevices(data);
    } catch (error) {
      if (credential === token) clearDevices(`Computers unavailable: ${error.message}`);
    } finally { devicesPending = false; $("devices-refresh").disabled = false; }
  }
  $("devices-refresh").addEventListener("click", loadDevices);
  $("devices-workspace").addEventListener("click", () => { closeDetails(); $("workspace-open").click(); });
  setInterval(() => {
    if (devicesExpiry && Date.now() >= devicesExpiry) clearDevices("Machine reports expired. Refresh to read current values.");
  }, 1000);

  // ---- API console ----
  function renderExample() {
    const config = endpointConfig, model = $("endpoint-model").value || "MODEL_ID";
    if (!config) { $("endpoint-example").textContent = ""; return; }
    const examples = {
      curl: `curl --fail-with-body "${config.openai_base_url}/chat/completions" \\\n` +
        `  -H "Authorization: Bearer $LUMABRI_API_KEY" \\\n  -H "Content-Type: application/json" \\\n` +
        `  -d '{"model":"${model}","messages":[{"role":"user","content":"Hello"}],"max_tokens":128}'`,
      openai: `import os\nfrom openai import OpenAI\n\nclient = OpenAI(\n    base_url="${config.openai_base_url}",\n` +
        `    api_key=os.environ["LUMABRI_API_KEY"],\n    max_retries=0,\n)\nreply = client.chat.completions.create(\n` +
        `    model="${model}",\n    messages=[{"role": "user", "content": "Hello"}],\n    max_tokens=128,\n)\n` +
        `print(reply.choices[0].message.content)`,
      anthropic: `import os\nfrom anthropic import Anthropic\n\nclient = Anthropic(\n    base_url="${config.anthropic_base_url}",\n` +
        `    api_key=os.environ["LUMABRI_API_KEY"],\n    max_retries=0,\n)\nwith client.messages.stream(\n` +
        `    model="${model}",\n    max_tokens=128,\n    messages=[{"role": "user", "content": "Hello"}],\n) as stream:\n` +
        `    for text in stream.text_stream:\n        print(text, end="", flush=True)`
    };
    $("endpoint-example").textContent = examples[$("endpoint-client").value] || examples.curl;
  }
  function renderEndpoint() {
    const config = endpointConfig;
    $("endpoint-openai").value = config?.openai_base_url || "";
    $("endpoint-anthropic").value = config?.anthropic_base_url || "";
    $("endpoint-scope").replaceChildren();
    if (config) {
      const scope = [];
      if (config.streaming === true) scope.push("Streaming");
      if (config.tools === true) scope.push("Tool calls");
      if (config.responses_api === true) scope.push(config.responses_storage === false ? "Responses API · not stored" : "Responses API");
      if (config.images) scope.push(`Inline images · up to ${config.images.max_images}, where the model has visual weights`);
      if (Array.isArray(config.anthropic_versions)) scope.push(`Anthropic versions ${config.anthropic_versions.join(", ")}`);
      for (const item of scope) $("endpoint-scope").append(element("li", "", item));
      $("endpoint-status").textContent = config.remote_reachability_verified === true ?
        "Connection settings for this service." :
        "Connection settings for this service. Reachability from other devices has not been verified.";
    }
    renderExample();
  }
  async function loadEndpoint() {
    if (endpointConfig || endpointPending || !token) return;
    if (busy) { $("endpoint-status").textContent = "Connection settings are read when no response is being generated. Switch back here afterwards."; return; }
    const credential = token;
    endpointPending = true; $("endpoint-status").textContent = "Reading the connection settings…";
    try {
      const config = await request("/api/v1/client-config");
      if (credential === token) { endpointConfig = config; renderEndpoint(); }
    } catch (error) {
      if (credential === token) $("endpoint-status").textContent = `Connection settings unavailable: ${error.message}`;
    } finally { endpointPending = false; }
  }
  function setEndpoint(open) {
    open = open && connected();
    app.classList.toggle("endpoint-mode", open); $("endpoint").hidden = !open;
    if (open) { closeDetails(); renderModel(); renderEndpoint(); loadEndpoint(); }
    syncChrome();
  }
  for (const control of document.querySelectorAll("[data-view]"))
    control.addEventListener("click", () => {
      if (control.dataset.view === "endpoint") { setEndpoint(true); return; }
      setEndpoint(false);
      // Return from the workspace to the conversation that was open.
      if (!$("workspace").hidden && !busy) { closeWorkspace(); syncChrome(); }
    });
  $("endpoint-client").addEventListener("change", renderExample);
  async function copy(id) {
    const source = $(id), text = "value" in source ? source.value : source.textContent;
    if (!text) return;
    try { await navigator.clipboard.writeText(text); toast("Copied"); }
    catch {
      // Clipboard access needs a secure context; fall back to a selection the user can copy.
      if ("select" in source) source.select(); else window.getSelection().selectAllChildren(source);
      toast("Selected — press Ctrl or ⌘ + C to copy");
    }
  }
  for (const control of document.querySelectorAll("[data-copy]"))
    control.addEventListener("click", () => copy(control.dataset.copy));
  // Leaving through the sidebar always lands on the view that app.js selects.
  for (const id of ["new-chat", "history", "workspace-open", "sign-out"])
    $(id).addEventListener("click", () => { if (app.classList.contains("endpoint-mode")) setEndpoint(false); });
  $("export").addEventListener("click", () => { if (current) toast("Conversation exported"); });

  // ---- approval of a load on this computer ----
  // The sharing service started without a screen keeps one request waiting for
  // this computer's owner. It is read here and decided with exactly the fence
  // it was displayed with. Nothing is chosen for the person: no focus is moved,
  // no key confirms, and an unanswered request simply expires as declined.
  const approvalIds = ["approval-decline", "approval-accept", "approval-repeat", "approval-revoke", "approval-dismiss"];
  const approvalProgress = new Set(["accepted", "loading", "segment_ready", "starting_host"]);
  let approvalShown = null, approvalSettled = null, approvalBusy = false, approvalRead = 0, approvalNote = "";
  let approvalMemory = null;
  const notLocked = request => request.memory_locked === false ?
    ` This load is larger than this computer's locked-memory allowance: it is held in RAM but not locked. ${approvalMemory?.detail || ""}` : "";
  function approvalFacts(request) {
    const rows = [
      ["Requested by", `${request.requester.slice(0, 24)}…`, true],
      ["Model", `${request.name} (${request.adapter})`],
      ["Layers", `${request.begin}–${request.end - 1} of ${request.layers}`],
      ["Memory", `${gib(request.ram_bytes)} kept in RAM · ${gib(request.metadata_bytes)} metadata headroom`],
      ["Limits", `${request.context} context · ${request.sessions} session slot${request.sessions === 1 ? "" : "s"} · ${request.threads} threads · up to ${request.max_new} new tokens per turn`],
      ["Request", `${request.allocation.slice(0, 12)}…`, true]];
    $("approval-facts").replaceChildren();
    for (const [label, value, mono] of rows) $("approval-facts").append(element("dt", "", label), element("dd", mono ? "mono" : "", value));
  }
  function renderApproval() {
    const box = $("approval"), shown = approvalShown, settled = approvalSettled;
    const show = buttons => { for (const id of approvalIds) { $(id).hidden = !buttons.includes(id); $(id).disabled = approvalBusy; } };
    box.hidden = !connected() || (!shown && !settled);
    box.classList.toggle("settled", !shown);
    box.classList.toggle("accepted", !shown && settled?.kind === "accepted");
    $("approval-status").textContent = approvalNote;
    if (shown) {
      const request = shown.request;
      $("approval-kicker").textContent = "approval needed";
      $("approval-title").textContent = `Load ${request.name} on this computer?`;
      $("approval-summary").textContent = request.expert_accelerator ? "Expert accelerator: approved layers only; weights remain in RAM." :
        request.runs_edge ? "This computer hosts the chat and receives the conversation text." :
        "This computer processes activations and keeps state for its layers.";
      approvalFacts(request);
      $("approval-hint").textContent = "Nothing loads unless you accept. Accepted weights stay in memory until you stop sharing. " +
        `An unanswered request is dropped after about ${Math.round(shown.seconds / 60)} minutes.` + notLocked(request);
      show(["approval-decline", "approval-accept", ...(request.repeat_supported ? ["approval-repeat"] : []), ...(shown.repeat ? ["approval-revoke"] : [])]);
    } else if (settled) {
      $("approval-kicker").textContent = settled.kind === "accepted" ? "accepted" : settled.kind === "declined" ? "declined" : "not loaded";
      $("approval-title").textContent = settled.name;
      $("approval-summary").textContent = settled.text;
      $("approval-facts").replaceChildren(); $("approval-hint").textContent = "";
      show([...(settled.repeat && settled.held ? ["approval-revoke"] : []), "approval-dismiss"]);
    }
  }
  function approvalApply(data) {
    const request = data.sharing ? data.request : null;
    approvalMemory = data.sharing && data.memory ? {computer: data.computer, ...data.memory} : null; renderMemory();
    const fence = request && {instance: data.instance, allocation: request.allocation, revision: data.revision};
    if (request && data.pending) {
      const repeat = approvalShown?.fence.allocation === request.allocation ? approvalShown.repeat : false;
      if (approvalShown?.fence.allocation !== request.allocation) approvalNote = "";
      approvalShown = {fence, request, seconds: data.decision_seconds, repeat};
      if (approvalSettled?.fence.allocation !== request.allocation) approvalSettled = null;
    } else {
      if (approvalShown) approvalNote = "";
      approvalShown = null;
      const settled = approvalSettled;
      if (settled?.kind === "accepted") {
        settled.held = request?.allocation === settled.fence.allocation;
        if (settled.held) {
          settled.fence = fence;
          settled.text = data.phase === "ready" ? "Loaded and ready on this computer. It appears in the model list once your sign-in has access to it." + notLocked(request) :
            approvalProgress.has(data.phase) ? `${data.state}.` : data.detail || `${data.state}.`;
        } else if (settled.seen) settled.text = "This load is no longer the one shown by this computer's sharing service. See Computers & models for its state.";
        settled.seen = settled.seen || settled.held;
      }
    }
    renderApproval();
  }
  async function approvalPoll() {
    if (!connected() || !management || !token || busy || approvalBusy || document.hidden || requestsActive) return;
    if (Date.now() - approvalRead < 2500) return;
    const credential = token;
    approvalRead = Date.now();
    try {
      const data = await request("/api/v1/donor/approval");
      if (credential === token && !approvalBusy) approvalApply(data);
    } catch {
      // Unavailable or not permitted from here: stay quiet and look again later.
      if (credential === token) { approvalRead = Date.now() + 12000; if (approvalShown) { approvalShown = null; approvalNote = ""; renderApproval(); } }
    }
  }
  async function approvalAct(action) {
    const target = action === "revoke_repeat" && !approvalShown ? approvalSettled : approvalShown;
    if (!target || approvalBusy || !management || !token) return;
    if (action === "allow_repeat" && !confirm("Allow this requester to load this same checkpoint and layers on this computer again for one hour without asking? One allocation at a time, with the limits shown. You can revoke it.")) return;
    const credential = token, name = approvalShown?.request.name || approvalSettled?.name;
    approvalBusy = true; approvalNote = "Sending your decision to this computer's sharing service…"; renderApproval();
    try {
      const answer = await request("/api/v1/donor/approval", "POST", {action, ...target.fence});
      if (credential !== token) return;
      approvalNote = "";
      if (action === "revoke_repeat") {
        target.repeat = false; approvalNote = answer.detail || "Repeat permission revoked."; record("Repeat-load permission revoked on this computer.");
      } else if (answer.result === "pending") {
        // A saved repeat permission that does not cover this request leaves the decision with the person.
        approvalNote = answer.detail; if (action === "allow_repeat") approvalShown.repeat = true;
      } else {
        const repeat = action === "allow_repeat" || approvalShown.repeat;
        approvalSettled = {fence: target.fence, name, repeat, held: answer.result === "accepted", seen: false,
          kind: answer.result === "accepted" ? "accepted" : answer.result === "declined" ? "declined" : "failed",
          text: answer.result === "accepted" ? (action === "allow_repeat" ? "Accepted, and repeat loads are allowed for one hour. " : "Accepted. ") + "Waiting for the plan to load." :
            answer.result === "declined" ? "Declined. No weights or model state were loaded." :
            answer.detail || "This computer could not reserve what the request needs. Nothing was loaded."};
        approvalShown = null;
        record(answer.result === "accepted" ? "Accepted a load on this computer." : answer.result === "declined" ? "Declined a load on this computer." : "A load on this computer was not accepted.");
      }
    } catch (error) {
      if (credential === token) approvalNote = error.message;
    } finally {
      approvalBusy = false; approvalRead = 0;
      if (credential === token) renderApproval();
    }
  }
  function approvalReset() { approvalShown = approvalSettled = approvalMemory = null; approvalNote = ""; approvalRead = 0; renderMemory(); renderApproval(); }
  $("approval-accept").addEventListener("click", () => approvalAct("accept"));
  $("approval-decline").addEventListener("click", () => approvalAct("decline"));
  $("approval-repeat").addEventListener("click", () => approvalAct("allow_repeat"));
  $("approval-revoke").addEventListener("click", () => approvalAct("revoke_repeat"));
  $("approval-dismiss").addEventListener("click", () => { approvalSettled = null; approvalNote = ""; renderApproval(); });
  setInterval(approvalPoll, 1000);

  // ---- keep the chrome in step with app.js ----
  function syncChrome() {
    const online = connected(), endpoint = app.classList.contains("endpoint-mode"), workspace = !$("workspace").hidden;
    $("conversation-title").textContent = endpoint ? "API access" : workspace ? "Computers & models" :
      current?.conversation.title || "New conversation";
    for (const control of document.querySelectorAll("[data-view]"))
      control.setAttribute("aria-pressed", String(control.dataset.view === "endpoint" ? endpoint : !endpoint && !workspace));
    $("devices-button").hidden = !operator || !online;
    $("devices-tab").hidden = !operator || !online;
    if (ROOM_FLOW_ENABLED && online) $("rooms").hidden = !management;
    if (!operator && panel === "devices" && !details.hidden) openDetails("model");
    if (online && !wasConnected) record("Connected to the local Lumabri service.");
    if (!online && wasConnected) {
      // Nothing from one account may remain visible to the next.
      closeDetails(); setEndpointOff(); endpointConfig = null; clearDevices(); approvalReset(); roomReset();
      $("activity-log").replaceChildren(); $("toasts").replaceChildren(); lastNotice = "";
      $("model-facts").replaceChildren(); $("model-list").replaceChildren();
      $("endpoint-model").value = ""; renderEndpoint(); $("endpoint-status").textContent = "";
    }
    wasConnected = online;
    if (!details.hidden && panel === "model") renderModel();
  }
  function setEndpointOff() { app.classList.remove("endpoint-mode"); $("endpoint").hidden = true; }
  function syncBusy() {
    const now = !$("stop").hidden;
    app.classList.toggle("generating", now);
    if (now && !wasBusy) record("Response started.");
    if (!now && wasBusy) record(`Response finished · ${$("generation-speed").textContent || "no timing reported"}.`);
    wasBusy = now;
  }
  function syncNotice() {
    const text = $("notice").textContent;
    if (text && text !== lastNotice && connected()) record(text);
    lastNotice = text;
  }
  new MutationObserver(syncChrome).observe(app, {attributes: true, attributeFilter: ["class"]});
  new MutationObserver(syncChrome).observe($("workspace"), {attributes: true, attributeFilter: ["hidden"]});
  new MutationObserver(syncChrome).observe($("history"), {childList: true});
  new MutationObserver(syncChrome).observe($("workspace-open"), {attributes: true, attributeFilter: ["hidden"]});
  new MutationObserver(() => { if (!details.hidden && panel === "model") renderModel(); }).observe($("model"), {childList: true});
  $("model").addEventListener("change", () => { if (!details.hidden || app.classList.contains("endpoint-mode")) renderModel(); });
  new MutationObserver(syncBusy).observe($("stop"), {attributes: true, attributeFilter: ["hidden"]});
  new MutationObserver(syncNotice).observe($("notice"), {childList: true, characterData: true, subtree: true});

  // ---- room flow (inactive unless ROOM_FLOW_ENABLED) ----
  const room = {id: "", timer: 0, owner: false, tracker: "", busy: false, waiting: 0};
  const roomSay = text => { $("room-status").textContent = text; };
  function roomClearCode() {
    clearInterval(room.timer); room.timer = 0; room.id = "";
    $("room-code-shown").textContent = ""; $("room-code-shown").hidden = true; $("room-cancel").hidden = true;
  }
  function roomControls() {
    const joining = $("mode-join").getAttribute("aria-pressed") === "true";
    for (const id of ["room-code-field", "room-address-field", "room-pledge", "room-consent-field", "room-join"]) $(id).hidden = !joining;
    $("room-create").hidden = joining || Boolean(room.id);
    $("room-create").disabled = room.busy || !room.owner;
    $("room-join").disabled = room.busy || !$("room-consent").checked || !$("room-code").value.trim();
    $("room-intro").textContent = !room.tracker ? "" : joining ?
      `This computer is in the household at ${room.tracker}. A code from another household moves it there.` : room.owner ?
      `This computer owns the household at ${room.tracker}. A code lets one other computer join it.` :
      `This computer joined the household at ${room.tracker}. Codes are created on the computer that owns it.`;
  }
  function setRoomMode(mode) {
    $("mode-create").setAttribute("aria-pressed", String(mode === "create"));
    $("mode-join").setAttribute("aria-pressed", String(mode === "join"));
    roomSay(""); roomControls();
  }
  function roomReset() {
    roomClearCode(); clearInterval(room.waiting); room.waiting = 0; room.busy = false; room.tracker = ""; room.owner = false;
    $("room-code").value = ""; $("room-address").value = ""; $("room-consent").checked = false; roomSay("");
    $("rooms").hidden = true;
  }
  async function roomRefresh() {
    if (!ROOM_FLOW_ENABLED || !management || !token || room.busy || room.waiting) return;
    const credential = token;
    try {
      const household = await request("/api/v1/household");
      if (credential !== token) return;
      room.tracker = household.tracker; room.owner = household.owner === true;
      if (!room.owner && !room.id) setRoomMode("join"); else roomControls();
    } catch (error) { roomSay(error.message); }
  }
  async function roomPoll() {
    if (!room.id || !token) return;
    const id = room.id;
    let state;
    try { state = await request(`/api/v1/household/pairing/${id}`); } catch { return; }
    if (id !== room.id) return;
    if (state.state === "waiting") {
      const left = `${Math.floor(state.expires_in / 60)}:${String(state.expires_in % 60).padStart(2, "0")}`;
      roomSay(`Type it on the other computer, under “I have a code”. Valid for ${left} more, for one computer.` +
        (state.failed ? ` ${state.failed} of ${state.max_attempts} wrong attempts so far.` : ""));
      return;
    }
    roomClearCode(); roomControls();
    const outcome = state.state === "used" ? `A computer joined from ${state.source || "an unknown address"} (identity ${state.joiner}). The code no longer works.` :
      state.state === "expired" ? "The code expired unused. Show a new one when the other computer is ready." :
      state.state === "locked" ? `The code was destroyed after ${state.failed} wrong attempts. Nobody joined. If that was not you, someone on this network is guessing.` :
      state.state === "cancelled" ? "The code was withdrawn. Nobody joined." : "The code was replaced or the household restarted. Nobody joined with it.";
    roomSay(outcome); record(outcome);
    if (state.state === "used") { clearDevices(); loadDevices(); }
  }
  async function roomCreate() {
    if (room.busy || room.id) return;
    room.busy = true; roomControls(); roomSay("");
    try {
      const made = await request("/api/v1/household/pairing", "POST", {action: "create"});
      room.id = made.id;
      $("room-code-shown").textContent = made.code; $("room-code-shown").hidden = false; $("room-cancel").hidden = false;
      roomSay(`Type it on the other computer, under “I have a code”. If that computer cannot find this household, its address is ${made.address}.`);
      room.timer = setInterval(roomPoll, 1000);
    } catch (error) { roomSay(error.message); }
    finally { room.busy = false; roomControls(); }
  }
  async function roomCancel() {
    const id = room.id;
    if (!id) return;
    roomClearCode(); roomControls();
    try { await request("/api/v1/household/pairing", "POST", {action: "cancel", id}); roomSay("The code was withdrawn. Nobody joined."); }
    catch (error) { roomSay(error.message); }
  }
  async function roomJoin() {
    if (room.busy || !$("room-consent").checked) return;
    const body = {code: $("room-code").value.trim()}, address = $("room-address").value.trim();
    const pledge = Math.round(Number($("room-gb").value));
    if (address) body.address = address;
    if (pledge >= 1) body.ram_gb = pledge;
    room.busy = true; roomControls(); roomSay("Checking the code with that household…");
    let answer;
    try { answer = await request("/api/v1/household/join", "POST", body); }
    catch (error) { room.busy = false; roomControls(); roomSay(error.message); return; }
    $("room-code").value = ""; $("room-consent").checked = false;
    if (!answer.restarting) {
      room.busy = false; roomControls();
      roomSay(`This computer joined the household at ${answer.tracker}, but Lumabri could not restart itself. In a terminal run: lumabri service stop, then lumabri start.`);
      return;
    }
    roomSay(`Joined the household at ${answer.tracker}. Lumabri is restarting on this computer…`);
    const credential = token, deadline = Date.now() + 120000;
    room.waiting = setInterval(async () => {
      if (credential !== token) { clearInterval(room.waiting); room.waiting = 0; return; }
      let household = null;
      try { household = await request("/api/v1/household"); } catch { /* the service is restarting */ }
      const back = household && household.tracker === answer.tracker && household.owner === false;
      if (!back && Date.now() < deadline) return;
      clearInterval(room.waiting); room.waiting = 0; room.busy = false;
      if (back) {
        room.tracker = household.tracker; room.owner = false; roomControls();
        const done = `This computer now shares with the household at ${answer.tracker}.`;
        roomSay(done); record(done); clearDevices(); loadDevices();
      } else {
        roomControls();
        roomSay("This computer joined, but Lumabri did not come back. In a terminal run: lumabri service stop, then lumabri start.");
      }
    }, 1500);
  }
  if (ROOM_FLOW_ENABLED) {
    $("mode-create").addEventListener("click", () => setRoomMode("create"));
    $("mode-join").addEventListener("click", () => setRoomMode("join"));
    const step = delta => {
      const field = $("room-gb"), value = Math.round(Number(field.value) || 1) + delta;
      field.value = String(Math.min(Number(field.max), Math.max(Number(field.min), value)));
    };
    $("room-gb-minus").addEventListener("click", () => step(-1));
    $("room-gb-plus").addEventListener("click", () => step(1));
    $("room-code").addEventListener("input", roomControls);
    $("room-consent").addEventListener("change", roomControls);
    $("room-create").addEventListener("click", roomCreate);
    $("room-cancel").addEventListener("click", roomCancel);
    $("room-join").addEventListener("click", roomJoin);
    roomControls();
  }

  syncChrome();
})();
