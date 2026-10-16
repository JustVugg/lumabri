"use strict";
// A second consumer of the owner's existing capacity API, not a browser planner.
// No generated prompts, credentials or observations go to browser storage.
window.createCapacityPanel = function ({session, request, card, choice, lock, explain}) {
  const $ = id => document.getElementById("capacity-" + id);
  let generation = 0, pending = false, working = false, abort = null, review = null;
  let allocations = [], rows = [""], records = [], routeModels = [];
  const status = text => { $("status").textContent = text; };
  const pretty = value => String(value || "unknown").replaceAll("_", " ");
  const ms = value => Number.isFinite(value) ? `${(value * 1000).toFixed(1)} ms` : "Not measured";
  const price = value => value ? `${value.currency} ${(value.micro_per_hour / 1000000).toFixed(4)}/h · declared whole machines, not a bill` : "Price unknown";
  function ticket() {
    const s = session(), version = generation;
    return () => {
      const now = session();
      return now.open && now.operator && now.token === s.token && now.epoch === s.epoch && version === generation;
    };
  }
  function invalidate() {
    review = null; routeModels = [];
    $("apply-form").hidden = true; $("route-models").replaceChildren();
  }
  function controls() {
    const s = session(), disabled = s.busy || !s.management;
    $("measure-details").hidden = !s.management;
    $("compare").hidden = !s.management;
    for (const id of ["form", "compare", "apply-form"]) $(id).inert = disabled;
    $("refresh").disabled = s.busy || pending;
    $("stop").hidden = !abort;
    $("add").disabled = disabled || rows.length >= 8;
    for (const button of $("records").querySelectorAll("button")) {
      button.hidden = !s.management; button.disabled = disabled;
    }
    if (!s.management) { abort?.abort(); invalidate(); }
  }
  function renderRows() {
    $("slots").replaceChildren();
    rows.forEach((value, i) => {
      const line = document.createElement("div"); line.className = "capacity-row";
      const label = document.createElement("label"); label.textContent = `Model ${i + 1}`;
      const select = document.createElement("select"); select.required = true;
      select.add(new Option("Choose an approved allocation", ""));
      for (const a of allocations) select.add(new Option(`${a.name} · ${a.id.slice(0, 12)}`, a.id));
      if (value && !allocations.some(a => a.id === value)) {
        const missing = new Option("Allocation no longer listed — choose again", value); missing.disabled = true; select.add(missing);
      }
      select.value = value; select.onchange = () => { rows[i] = select.value; };
      label.append(select); line.append(label);
      if (rows.length > 1) {
        const remove = document.createElement("button"); remove.type = "button";
        remove.className = "text-button"; remove.textContent = `Remove model ${i + 1}`;
        remove.onclick = () => { rows.splice(i, 1); renderRows(); controls(); }; line.append(remove);
      }
      $("slots").append(line);
    });
  }
  function syncWorkspace(data) {
    // Do not rebuild an editor on every 10-second inventory poll.
    const next = data.allocations.map(a => ({id: a.id, name: a.name}));
    if (JSON.stringify(next) !== JSON.stringify(allocations) && !working) { allocations = next; renderRows(); }
  }
  function limits() {
    const number = id => {
      const input = $(id), n = Number(input.value);
      if (!input.checkValidity() || !Number.isSafeInteger(n)) throw new Error("Review the integer limits in the form.");
      return n;
    };
    return {ttft_ms: number("ttft"), gap_ms: number("gap"), max_age_seconds: number("age")};
  }
  function observation(parent, name, data, historical = false) {
    const facts = [["Evidence", historical ? "Historical — not revalidated" : pretty(data.state)],
      ["Measured", new Date(data.measured_at * 1000).toLocaleString()],
      ["Workload", `${data.clients} clients · ${data.rounds} rounds · ${data.max_tokens} output tokens per request`],
      ["Capacity", "Short observed workload; not production certification"]];
    if (!historical) facts.push(["Machine footprint price", price(data.cost)]);
    for (const [i, model] of data.models.entries()) facts.push(
      [`Model ${i + 1} allocation`, model.allocation],
      [`Model ${i + 1} outcomes`, `${model.completed}/${model.requests} completed · ${model.rejected} rejected · ${model.failed} failed`],
      [`Model ${i + 1} first text / gap p95`, `${ms(model.ttft_p95_seconds)} / ${ms(model.gap_p95_of_response_p95_seconds)}`]);
    return card(parent, name, facts);
  }
  function renderRecords() {
    const baseline = $("baseline").value;
    const selected = new Set(Array.from($("candidates").querySelectorAll("input:checked"), e => e.value));
    $("records").replaceChildren(); $("baseline").replaceChildren(new Option("Choose a record", ""));
    $("candidates").replaceChildren();
    for (const record of records) {
      const item = record.observation ? observation($("records"), record.name, record.observation, true) :
        card($("records"), record.name, [["Evidence", "Record unavailable"]]);
      if (record.observation?.state === "incomplete") {
        const p = document.createElement("p"); p.textContent = "Incomplete — cannot establish capacity."; item.append(p);
      }
      const actions = document.createElement("div"); actions.className = "allocation-actions";
      for (const [label, run] of [["Check limits", () => check(record.name)], ["Remove record", () => remove(record.name)]]) {
        const button = document.createElement("button"); button.className = "text-button"; button.textContent = label;
        button.onclick = run; actions.append(button);
      }
      item.append(actions);
      $("baseline").add(new Option(record.name, record.name));
      choice($("candidates"), record.name, record.name);
      $("candidates").lastElementChild.querySelector("input").checked = selected.has(record.name);
    }
    if (!records.length) $("records").textContent = "No saved measurements. Prepare and approve models before measuring them.";
    if (records.some(record => record.name === baseline)) $("baseline").value = baseline;
    controls();
  }
  async function refresh() {
    const s = session();
    if (!s.open || !s.operator || !s.token || s.busy || pending || document.hidden) return;
    pending = true; const valid = ticket(); controls();
    try {
      const data = await request("/api/v1/capacity");
      if (valid()) { records = data.records; renderRecords(); }
    } catch (error) {
      if (valid()) { records = []; renderRecords(); status(`Measurements unavailable: ${error.message}`); }
    } finally { pending = false; controls(); }
  }
  async function task(fn) {
    const s = session();
    if (!s.open || !s.management || !s.token || s.busy) return;
    generation++; working = true; lock(true); controls();
    const valid = ticket();
    try { await fn(valid); }
    catch (error) { if (valid()) status(error.message); }
    finally { working = false; lock(false); controls(); }
  }
  async function prepareApply(name, data, bounds, valid) {
    invalidate();
    if (data.state !== "observed_workload_passed") return;
    if (!/^[0-9a-f]{64}$/.test(data.observation_id))
      throw new Error("The runtime did not identify the exact observation. Update the complete runtime before applying a plan.");
    const workspace = await request("/api/v1/workspace");
    if (!valid() || !session().management) return;
    if (!workspace.registry_ok) throw new Error("Model registry unavailable. Routing has not changed.");
    routeModels = workspace.models;
    review = {record: name, observation_id: data.observation_id, bounds, count: data.models.length};
    for (const [i, model] of data.models.entries()) {
      const label = document.createElement("label");
      label.textContent = `Model ${i + 1} → allocation ${model.allocation.slice(0, 12)}`;
      const select = document.createElement("select"); select.required = true;
      select.add(new Option("Choose the corresponding managed model", ""));
      for (const m of routeModels) select.add(new Option(`${m.name} · revision ${m.revision}`, m.id));
      label.append(select); $("route-models").append(label);
    }
    $("apply-form").hidden = false;
  }
  async function check(name) {
    return task(async valid => {
      invalidate(); $("result").replaceChildren(); const bounds = limits();
      status(`Checking ${name} against the current runtime…`);
      const data = await request("/api/v1/capacity/check", "POST", {record: name, ...bounds});
      if (!valid()) return;
      observation($("result"), name, data);
      status(`${name}: ${pretty(data.state)}. Checked now; applying will revalidate again.`);
      await prepareApply(name, data, bounds, valid);
    });
  }
  async function remove(name) {
    return task(async valid => {
      if (!confirm(`Remove measurement “${name}”? Only this observation is deleted. Models, RAM and conversations are unchanged.`)) return;
      invalidate(); $("result").replaceChildren();
      try { await request("/api/v1/capacity/remove", "POST", {record: name}); }
      catch (error) { throw new Error(`${error.message} Refresh records before retrying; no automatic retry was sent.`); }
      if (!valid()) return;
      records = records.filter(record => record.name !== name); renderRecords(); status("Measurement removed. Weights are unchanged.");
    });
  }
  async function consume(response, rounds, valid) {
    if (!response.headers.get("Content-Type")?.startsWith("text/event-stream")) throw new Error("Invalid measurement stream type");
    const reader = response.body.getReader(), decoder = new TextDecoder("utf-8", {fatal: true});
    let pending = "", result = null, completed = -1, events = 0;
    try {
      for (;;) {
        const chunk = await reader.read();
        pending += chunk.done ? decoder.decode() : decoder.decode(chunk.value, {stream: true});
        let end;
        while ((end = pending.indexOf("\n\n")) !== -1) {
          if (end > 65536 || ++events > rounds + 3 || result) throw new Error("Invalid measurement stream length");
          const lines = pending.slice(0, end).split("\n"); pending = pending.slice(end + 2);
          if (lines.length !== 2 || !lines[0].startsWith("event: ") || !lines[1].startsWith("data: ")) throw new Error("Invalid measurement stream");
          const kind = lines[0].slice(7), data = JSON.parse(lines[1].slice(6));
          if (kind === "progress") {
            if (data.weights_changed !== false || data.rounds !== rounds || data.completed_rounds !== completed + 1 ||
                data.completed_rounds > rounds) throw new Error("Invalid measurement progress");
            completed = data.completed_rounds;
            if (valid()) { $("progress").value = completed; status(`Measurement: ${completed}/${rounds} rounds completed. Existing models stay loaded.`); }
          } else if (kind === "result" && completed >= 0 && ["measured", "incomplete"].includes(data.state) &&
              data.rounds === rounds && Array.isArray(data.models) && data.models.length >= 1 && data.models.length <= 8) {
            result = data;
          } else if (kind === "error") throw new Error(explain(data.error || "Measurement failed"));
          else throw new Error("Unknown measurement event");
        }
        if (pending.length > 65536) throw new Error("Measurement frame too large");
        if (chunk.done) break;
      }
      if (!result || pending) throw new Error("Connection closed before the measurement completed");
      return result;
    } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
  }
  $("form").onsubmit = event => {
    event.preventDefault();
    task(async valid => {
      if (!rows.length || rows.some(id => !allocations.some(a => a.id === id)) || new Set(rows).size !== rows.length)
        throw new Error("Choose distinct, currently listed allocations in model order.");
      const body = {record: $("name").value.trim(), clients: Number($("clients").value), rounds: Number($("rounds").value),
        max_tokens: Number($("tokens").value), allocations: [...rows]};
      if (!$("form").checkValidity() || body.clients < rows.length) throw new Error("Each selected model needs at least one client. Review the workload limits.");
      if (!confirm(`Run ${body.rounds} rounds with ${body.clients} concurrent clients? This performs real inference and competes with active chats. No weights will be loaded or released.`)) return;
      invalidate(); $("result").replaceChildren();
      abort = new AbortController(); const own = abort, credential = session().token;
      const timer = setTimeout(() => own.abort(), 300000);
      $("progress").hidden = false; $("progress").max = body.rounds; $("progress").value = 0;
      status("Waiting for runtime validation. No weights will be changed."); controls();
      try {
        const response = await fetch("/api/v1/capacity/measure", {method: "POST", cache: "no-store", signal: own.signal,
          headers: {Authorization: `Bearer ${credential}`, "Content-Type": "application/json"}, body: JSON.stringify(body)});
        if (!response.ok) { const data = await response.json(); throw new Error(explain(data.error || `Request failed (${response.status})`)); }
        const data = await consume(response, body.rounds, valid);
        if (!valid()) return;
        observation($("result"), body.record, data);
        status(`Measurement ${pretty(data.state)}. Check limits before using it for routing; this is not production certification.`);
      } catch (error) {
        if (valid()) status(`${own.signal.aborted ? "Measurement stopped or timed out." : error.message} Refresh records after cleanup to inspect any incomplete observation. Weights were not released.`);
      } finally {
        clearTimeout(timer); if (abort === own) abort = null;
        if (valid()) $("progress").hidden = true;
        controls();
      }
    });
  };
  $("compare").oninput = () => { invalidate(); $("result").replaceChildren(); };
  $("compare").onsubmit = event => {
    event.preventDefault();
    task(async valid => {
      invalidate(); $("result").replaceChildren(); const bounds = limits(), baseline = $("baseline").value;
      const names = [baseline, ...Array.from($("candidates").querySelectorAll("input:checked"), e => e.value).filter(n => n !== baseline)];
      if (!baseline || names.length < 2 || names.length > 8) throw new Error("Choose a baseline and one to seven other records.");
      status("Revalidating measured plans…");
      const data = await request("/api/v1/capacity/select", "POST", {...bounds, records: names});
      if (!valid()) return;
      for (const candidate of data.candidates) card($("result"), candidate.name, [
        ["Current check", pretty(candidate.state)], ["Whole-machine price", price(candidate.declared_machine_cost)]]);
      status(data.selected ? `Selected ${data.selected}. ${pretty(data.objective)}. Routing has not changed.` : "No comparable plan passed the declared limits. Routing has not changed.");
      if (data.selected) {
        const checked = await request("/api/v1/capacity/check", "POST", {record: data.selected, ...bounds});
        if (valid()) await prepareApply(data.selected, checked, bounds, valid);
      }
    });
  };
  $("apply-form").onsubmit = event => {
    event.preventDefault();
    task(async valid => {
      const selected = review;
      if (!selected) return;
      const models = Array.from($("route-models").querySelectorAll("select"), e => routeModels.find(m => m.id === e.value));
      if (models.length !== selected.count || models.some(m => !m) || new Set(models.map(m => m.id)).size !== models.length)
        throw new Error("Choose a distinct managed model for each allocation, in the displayed order.");
      if (!confirm(`Apply measurement “${selected.record}” to ${models.map(m => m.name).join(", ")}?\n\nReplace their replica routes for future turns. Active turns continue. Existing weights stay loaded; this does not release RAM or stop servers.`)) return;
      const body = {record: selected.record, observation_id: selected.observation_id, ...selected.bounds,
        models: models.map(m => ({id: m.id, revision: m.revision}))};
      invalidate(); // A lost acknowledgement must never cause a blind resubmit.
      try {
        const data = await request("/api/v1/capacity/apply", "POST", body);
        if (valid()) status(data.applied ? "Routing applied atomically to future turns. Weights and active conversations are unchanged. Refresh the workspace to inspect revisions." : `Routing not applied: ${pretty(data.state)}.`);
      } catch (error) {
        throw new Error(`${error.message} Inspect model revisions before retrying; the operation may already have applied. No automatic retry was sent.`);
      }
    });
  };
  $("refresh").onclick = refresh;
  $("stop").onclick = () => abort?.abort();
  $("add").onclick = () => { if (rows.length < 8) { rows.push(""); renderRows(); controls(); } };
  function close() {
    generation++; abort?.abort(); invalidate(); allocations = []; records = []; rows = [""];
    $("result").replaceChildren(); $("records").replaceChildren(); $("candidates").replaceChildren();
    $("baseline").replaceChildren(new Option("Choose a record", ""));
    $("name").value = ""; $("measure-details").open = false; $("progress").hidden = true;
    renderRows(); status("");
  }
  renderRows();
  return {controls, syncWorkspace, refresh, close, active: () => working};
};
