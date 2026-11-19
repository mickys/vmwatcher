export const esc = (value) =>
  String(value ?? "").replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
export function filtered(
  vms,
  { query = "", showOff = false, pinnedOnly = false, selected = new Set() },
) {
  return vms.filter(
    (vm) =>
      (showOff || ["running", "paused"].includes(vm.state)) &&
      (!pinnedOnly || selected.has(vm.id)) &&
      vm.name.toLowerCase().includes(query.toLowerCase()),
  );
}

// Rows needed when tiles of the given widths (in slots) are placed in order
// into a grid of `columns` slots, the way CSS grid auto-placement does.
export function rowsFor(spans, columns) {
  let rows = 1,
    used = 0;
  for (const span of spans) {
    const size = Math.min(span, columns);
    if (used + size > columns) {
      rows++;
      used = 0;
    }
    used += size;
  }
  return rows;
}

// Pick the grid that displays the most image area, using actual monitor ratios.
export function wallGrid(aspects, width, height, gap = 6) {
  let best = { columns: 1, rows: Math.max(1, aspects.length) }, area = -1;
  for (let columns = 1; columns <= aspects.length; columns++) {
    const rows = Math.ceil(aspects.length / columns);
    const cellWidth = Math.max(0, (width - gap * (columns - 1)) / columns);
    const cellHeight = Math.max(0, (height - gap * (rows - 1)) / rows);
    const shown = aspects.reduce((sum, aspect) => {
      const w = Math.min(cellWidth, cellHeight * aspect);
      return sum + w * w / aspect;
    }, 0);
    if (shown > area) { best = { columns, rows }; area = shown; }
  }
  return best;
}

export const isActive = (vm) => ["running", "paused"].includes(vm.state);
export const canLive = (vm) => !!vm.live && isActive(vm);
export function liveNote(vm, encoder = "vnc") {
  if (!isActive(vm)) return "The VM is not running";
  if (encoder !== "vnc" && vm.heads > 0) return "";
  return vm.live ? "" : vm.liveNote || "Live view is unavailable for this VM";
}
const stateRank = (state) => (state === "running" ? 0 : state === "paused" ? 1 : 2);
export const byState = (a, b) => stateRank(a.state) - stateRank(b.state) || a.name.localeCompare(b.name);

const keyPattern = /^[a-z0-9-]{1,32}\/[0-9a-f-]{36}$/;
// Selections saved before multi-host support hold bare VM identifiers; those
// VMs now live on the host called "local".
export function parseKeys(text) {
  try {
    const data = JSON.parse(text || "[]");
    if (!Array.isArray(data)) return [];
    return data
      .map((id) => (typeof id === "string" && /^[0-9a-f-]{36}$/.test(id) ? "local/" + id : id))
      .filter((id) => typeof id === "string" && keyPattern.test(id));
  } catch {
    return [];
  }
}

// How many monitors a combined preview shows, judged by its shape.
export function monitorsOf(width, height) {
  return height ? Math.max(1, Math.min(4, Math.round(width / height / 1.6))) : 1;
}
