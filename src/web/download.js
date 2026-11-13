"use strict";
// "Get a model": shown in the chat view when this computer has no model yet.
// It has three faces: a model that fits the household's memory and can be
// downloaded; a model that is too large for the computers taking part; and
// no downloadable model at all. A download starts only when the person
// presses Download. Afterwards the usual catalogue and Prepare models flow
// takes over; nothing is prepared or loaded from here. Reads state owned by
// app.js and keeps nothing in URLs or browser storage.
(() => {
  const app = $("app"), card = $("get-model");
  let data = null, pending = false, timer = 0, wasConnected = false;
  let chosen = "", finished = "", note = "";

  Object.assign(errors, {
    downloads_local_gateway_only: "Models can be downloaded only from the page on the computer that runs Lumabri.",
    download_not_found: "This model is no longer offered. Refresh the page.",
    download_already_running: "A download is already running on this computer.",
    another_download_is_running: "Another download is running on this computer.",
    model_already_present: "Something with this model's name is already in the models folder.",
    model_too_large_for_household: "Your computers do not offer enough memory for this model. Add a computer to use it.",
    household_memory_unknown: "The memory offered by your computers could not be read just now. Try again in a moment.",
    insufficient_disk_space: "There is not enough free space in the models folder.",
    models_folder_unavailable: "The models folder cannot be opened. Choose a folder in Lumabri's settings.",
    downloader_unavailable: "The download helper (lumabri-fetch) is missing or cannot run on this computer.",
    download_catalogue_invalid: "The list of downloadable models on this computer cannot be read.",
    download_state_unavailable: "The private download record on this computer cannot be used."
  });
  const kept = " What was already downloaded is kept.";
  const reasons = {
    checksum_mismatch: "A downloaded file did not match its published checksum, so it was removed. Nothing was added to your models. Try again to fetch that file once more.",
    size_mismatch: "The server offered a file of a different size than it had listed. That file was not kept.",
    insufficient_disk_space: "There was not enough free space in the models folder." + kept,
    tls_verification_failed: "The server's certificate could not be verified, so nothing was downloaded from it.",
    network_unreachable: "The server could not be reached. Check the connection and try again." + kept,
    network_stalled: "The connection stalled. Try again." + kept,
    transfer_interrupted: "The connection was interrupted. Try again." + kept,
    http_server_error: "The server answered with an error. Try again later." + kept,
    too_many_redirects: "The server redirected too many times. Nothing was downloaded from it.",
    repository_or_file_not_found: "The model is no longer at its published address.",
    repository_access_denied: "The server refused access to this model.",
    listed_path_refused: "The model's file list names a file outside its own folder, so the download was refused.",
    model_already_present: "Something with this model's name is already in the models folder."
  };

  const element = (tag, className, text) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  };
  // Sizes as the model publishers state them: GB of 10^9 bytes.
  const amount = bytes => !Number.isFinite(bytes) ? "unknown" :
    bytes >= 1e9 ? `${(bytes / 1e9).toFixed(bytes >= 2e10 ? 0 : 1)} GB` : `${Math.max(1, Math.round(bytes / 1e6))} MB`;
  const moving = model => model.state === "downloading";
  const here = model => model.state === "present" || model.state === "conflict";
  const connected = () => app.classList.contains("connected");

  // The model the card is about: the one being downloaded, the one the person
  // picked, the first that fits, and only then the first that is too large.
  function subject() {
    if (!data) return null;
    const open = data.models.filter(model => !here(model));
    return data.models.find(moving) || data.models.find(model => model.id === chosen) ||
      open.find(model => model.state !== "absent") || open.find(model => model.fits === true) || open[0] || null;
  }
  // The card is for a computer with no model at all, and for a download that
  // this page started or found running.
  function wanted() {
    if (!data || !management || !connected()) return false;
    if (data.models.some(moving) || finished) return true;
    return models.length === 0 && data.folder_ok && data.local_entries === 0;
  }
  function fact(list, label, value, href) {
    const term = element("dt", "", label), detail = element("dd");
    if (href && /^https:\/\//.test(href)) {
      const link = element("a", "", value);
      link.href = href; link.target = "_blank"; link.rel = "noopener noreferrer";
      detail.append(link);
    } else detail.textContent = value;
    list.append(term, detail);
  }
  function remaining(model) {
    const rate = model.speed_bytes_per_second;
    if (!(rate > 0)) return "";
    const seconds = (model.size_bytes - model.bytes) / rate;
    return seconds < 90 ? " · less than 2 minutes left" : seconds < 5400 ? ` · about ${Math.round(seconds / 60)} minutes left` :
      ` · about ${(seconds / 3600).toFixed(1)} hours left`;
  }
  const needs = model => `Needs about ${amount(model.memory_needed_bytes)} of memory across your computers.`;
  function others(shown) {
    const list = $("get-model-others"); list.replaceChildren();
    const rest = data.models.filter(model => model !== shown && !here(model));
    list.hidden = !rest.length || data.models.some(moving);
    if (list.hidden) return;
    list.append(element("div", "side-k", "OTHER MODELS"));
    for (const model of rest) {
      const row = element("div", "get-model-other"), text = element("div");
      text.append(element("strong", "", model.name), element("span", "", ` · ${amount(model.size_bytes)} download`));
      text.append(element("p", "", model.fits === true ? `${needs(model)} Your computers offer enough.` :
        model.fits === false ? `${needs(model)} Add a computer to use it.` : needs(model)));
      row.append(text); row.classList.toggle("too-large", model.fits !== true);
      if (model.fits === true) {
        const choose = element("button", "text-button", "Show");
        choose.type = "button"; choose.disabled = pending;
        choose.setAttribute("aria-label", `Show ${model.name}`);
        choose.addEventListener("click", () => { chosen = model.id; note = ""; render(); });
        row.append(choose);
      }
      list.append(row);
    }
  }
  function render() {
    clearTimeout(timer); timer = 0;
    const show = wanted();
    card.hidden = !show;
    if (!show) return;
    const model = subject(), facts = $("get-model-facts"), progress = $("get-model-progress");
    const download = $("get-model-download"), cancel = $("get-model-cancel"), open = $("get-model-open");
    const done = finished && data.models.find(item => item.id === finished && item.state === "present");
    const shown = done || model;
    facts.replaceChildren(); $("get-model-others").replaceChildren(); $("get-model-others").hidden = true;
    progress.hidden = true; cancel.hidden = true; open.hidden = true; download.hidden = true;
    download.disabled = pending; cancel.disabled = pending; cancel.textContent = "Cancel download";
    card.classList.toggle("is-downloading", Boolean(shown && moving(shown)));
    card.classList.remove("is-too-large");
    $("get-model-title").textContent = "Get a model";
    $("get-model-next").textContent = "When it is ready the model appears under Computers & models → Prepare models. " +
      "Nothing is loaded automatically: you choose the computers, and each one still approves.";
    const free = Number.isFinite(data.free_bytes) ? `${amount(data.free_bytes)} free in ${data.folder}` : data.folder;
    if (!shown) {
      // No downloadable model: say how to bring one without a download.
      $("get-model-intro").textContent = "Lumabri needs a model to chat with. No model is available for download yet.";
      fact(facts, "Bring your own", `Copy a model folder or a GGUF file into ${data.folder}`);
      fact(facts, "Free space", free);
      $("get-model-status").textContent = "";
      $("get-model-next").textContent = "A model placed there appears under Computers & models → Prepare models. Nothing is loaded automatically.";
      return;
    }
    const offered = Number.isFinite(data.offered_memory_bytes) ? amount(data.offered_memory_bytes) : null;
    const fits = shown.fits === true, tooLarge = shown.fits === false && !done && !moving(shown);
    $("get-model-intro").textContent = done ? "Your model is on this computer." : tooLarge ?
      "Lumabri needs a model to chat with. The one available for download is larger than the memory your computers offer." :
      "Lumabri needs a model to chat with. It is downloaded once and stays on this computer.";
    fact(facts, "Model", shown.summary ? `${shown.name} · ${shown.summary}` : shown.name);
    fact(facts, "Download size", amount(shown.size_bytes));
    fact(facts, "Memory needed", `about ${amount(shown.memory_needed_bytes)} across your computers` +
      (offered ? ` · they offer ${offered}` : "") + " · an estimate until the model is on disk");
    if (shown.licence) fact(facts, "Licence", shown.licence, shown.licence_url);
    if (shown.page_url) fact(facts, "Published at", shown.page_url.replace(/^https:\/\//, ""), shown.page_url);
    if (shown.commit) fact(facts, "Revision", shown.commit.slice(0, 12));
    fact(facts, "Free space", free);
    let status = note;
    if (done) {
      status = "Downloaded and checked. It now appears under Prepare models.";
      open.hidden = $("workspace-open").hidden;
    } else if (moving(shown)) {
      progress.hidden = false; progress.max = shown.size_bytes; progress.value = Math.min(shown.bytes, shown.size_bytes);
      cancel.hidden = false;
      const percent = Math.min(100, Math.floor(100 * shown.bytes / shown.size_bytes));
      status = `${amount(shown.bytes)} of ${amount(shown.size_bytes)} · ${percent}%` +
        (shown.speed_bytes_per_second > 0 ? ` · ${(shown.speed_bytes_per_second / 1e6).toFixed(1)} MB/s` : "") + remaining(shown);
      timer = setTimeout(load, 1000);
    } else if (tooLarge) {
      card.classList.add("is-too-large");
      download.hidden = false; download.disabled = true; download.textContent = "Download";
      status = `${needs(shown)} Add a computer to use it.`;
    } else {
      download.hidden = false;
      const missing = shown.size_bytes - shown.bytes + data.reserve_bytes - data.free_bytes;
      if (shown.state === "interrupted" || (shown.state === "failed" && shown.bytes > 0)) {
        download.textContent = shown.state === "failed" ? "Try again" : "Resume download";
        cancel.hidden = false; cancel.textContent = "Discard what was downloaded";
        status = status || (shown.state === "failed" ? reasons[shown.error] || `The download did not finish (${explain(shown.error)}).` :
          shown.bytes ? `Stopped at ${amount(shown.bytes)} of ${amount(shown.size_bytes)}.${kept}` : `The download was stopped.${kept}`);
      } else {
        download.textContent = shown.state === "failed" ? "Try again" : "Download";
        if (shown.state === "failed") status = status || reasons[shown.error] || `The download did not finish (${explain(shown.error)}).`;
      }
      if (!data.downloader_available) { download.disabled = true; status = errors.downloader_unavailable; }
      else if (!fits) { download.disabled = true; status = errors.household_memory_unknown; }
      else if (Number.isFinite(data.free_bytes) && missing > 0) {
        download.disabled = true; status = `Free about ${amount(missing)} more in the models folder to download this model.`;
      }
    }
    $("get-model-status").textContent = status;
    if (!done) others(shown);
  }
  function accept(answer) {
    const before = data && subject();
    data = answer;
    const now = before && data.models.find(model => model.id === before.id);
    if (before && moving(before) && now?.state === "present") finished = now.id;
    render();
  }
  // One request at a time, none while a response is being generated (that
  // turn needs both of this account's request permits) and none while the
  // workspace is open.
  async function load() {
    clearTimeout(timer); timer = 0;
    if (!token || !management || !connected() || pending) return;
    // The card is not on screen in the workspace, whose own requests need the
    // permits; closing the workspace reads the state again.
    if (!$("workspace").hidden) return;
    if (busy || managementBusy || document.hidden) { if (data?.models.some(moving)) timer = setTimeout(load, 2000); return; }
    const credential = token;
    pending = true;
    try {
      const answer = await request("/api/v1/downloads");
      pending = false;
      if (credential === token) accept(answer);
    } catch (error) {
      if (credential === token && data?.models.some(moving)) timer = setTimeout(load, 3000);
    } finally { pending = false; }
  }
  async function act(action) {
    const model = subject();
    if (!model || pending || !token) return;
    const credential = token;
    pending = true; chosen = model.id; note = ""; render();
    try {
      const answer = await request("/api/v1/downloads", "POST", {action, id: model.id});
      if (credential !== token) return;
      if (action === "cancel") { note = "Download cancelled. What was downloaded has been removed."; finished = ""; }
      pending = false; accept(answer);
    } catch (error) {
      if (credential === token) { pending = false; note = error.message; render(); }
    } finally { pending = false; }
  }
  $("get-model-download").addEventListener("click", () => act("start"));
  $("get-model-cancel").addEventListener("click", () => act("cancel"));
  $("get-model-open").addEventListener("click", () => { finished = ""; chosen = ""; render(); $("workspace-open").click(); });

  function reset() {
    clearTimeout(timer); timer = 0;
    data = null; chosen = ""; finished = ""; note = ""; pending = false; card.hidden = true;
    $("get-model-facts").replaceChildren(); $("get-model-others").replaceChildren(); $("get-model-status").textContent = "";
  }
  function sync() {
    const online = connected();
    if (online && !wasConnected) {
      // A computer that already has approved models needs no card and no request.
      if (management && models.length === 0) load();
    } else if (!online && wasConnected) reset();
    wasConnected = online;
  }
  new MutationObserver(sync).observe(app, {attributes: true, attributeFilter: ["class"]});
  // The model list changed (Refresh, or a model was approved): decide again from what is known.
  new MutationObserver(() => { if (data) render(); }).observe($("model"), {childList: true});
  $("refresh").addEventListener("click", () => setTimeout(() => { if (connected() && management && (data || models.length === 0)) load(); }, 800));
  new MutationObserver(() => {
    if (!$("workspace").hidden || !connected() || !management) return;
    if (data || models.length === 0) load();
  }).observe($("workspace"), {attributes: true, attributeFilter: ["hidden"]});
  document.addEventListener("visibilitychange", () => { if (!document.hidden && data?.models.some(moving)) load(); });
  sync();
})();
