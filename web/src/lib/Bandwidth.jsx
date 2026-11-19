import { createContext, useContext, useEffect, useState } from "react";
import { api } from "./api.js";

const Rates = createContext(null);

export function BandwidthProvider({ children }) {
  const [rates, setRates] = useState(null);
  useEffect(() => {
    let stopped = false, timer, previous, lastTime;
    async function poll() {
      try {
        const totals = await api("/api/bandwidth");
        if (stopped) return;
        const now = performance.now();
        if (previous && now > lastTime) {
          const next = {};
          for (const [scope, count] of Object.entries(totals)) {
            const old = previous[scope];
            const up = Math.max(0, count.up - (old?.up ?? 0)) * 8000 / (now - lastTime);
            const down = Math.max(0, count.down - (old?.down ?? 0)) * 8000 / (now - lastTime);
            for (const key of new Set(["total", scope, scope.split("/").slice(0, 2).join("/")])) {
              next[key] ??= { up: 0, down: 0 };
              next[key].up += up;
              next[key].down += down;
            }
          }
          setRates(next);
        }
        previous = totals;
        lastTime = now;
      } catch {
        previous = null;
        if (!stopped) setRates(null);
      }
      if (!stopped) timer = setTimeout(poll, 2000);
    }
    poll();
    return () => { stopped = true; clearTimeout(timer); };
  }, []);
  return <Rates.Provider value={rates}>{children}</Rates.Provider>;
}

function format(rate) {
  if (rate < 1000) return Math.round(rate) + " b/s";
  if (rate < 1e6) return (rate / 1000).toFixed(1) + " kb/s";
  return (rate / 1e6).toFixed(1) + " Mb/s";
}
export function Bandwidth({ scope = "total" }) {
  const rates = useContext(Rates);
  const count = rates?.[scope];
  return <span className="bandwidth" title="Download and upload rates from this browser tab, in bits per second">
    ↓ {rates ? format(count?.down ?? 0) : "—"} · ↑ {rates ? format(count?.up ?? 0) : "—"}
  </span>;
}
