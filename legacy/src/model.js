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
