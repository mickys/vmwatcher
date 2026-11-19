// A page instance has its own counters, even when several tabs share a login.
const tab = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, (c) => {
  const n = crypto.getRandomValues(new Uint8Array(1))[0] & 15;
  return (c === "x" ? n : (n & 3) | 8).toString(16);
});
export function viewerUrl(path) {
  const url = new URL(path, location.href);
  url.searchParams.set("tab", tab);
  return url.pathname + url.search;
}

let unauthorized = () => {};
export function onUnauthorized(handler) {
  unauthorized = handler;
}

export async function api(url, options = {}) {
  const init = { ...options };
  if (init.body !== undefined && typeof init.body !== "string") {
    init.body = JSON.stringify(init.body);
    init.headers = { "Content-Type": "application/json", ...init.headers };
  }
  let response;
  try {
    response = await fetch(viewerUrl(url), init);
  } catch {
    throw Error("The VM Watcher service is unreachable");
  }
  let data;
  try {
    data = await response.json();
  } catch {
    throw Error("Server response unavailable");
  }
  if (response.status === 401 && !url.startsWith("/api/login")) unauthorized();
  if (!response.ok) throw Object.assign(Error(data.error || "Request failed"), { status: response.status });
  return data;
}

export const vmUrl = (vm, rest) => viewerUrl(`/api/hosts/${vm.host}/vms/${vm.id}/${rest}`);

export function wsUrl(path) {
  const url = new URL(path, location.href);
  url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
  return url.href;
}
