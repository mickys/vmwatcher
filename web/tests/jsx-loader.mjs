// Lets `node --test` import the app's .jsx and .css files directly.
import { registerHooks } from "node:module";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { transformSync } from "esbuild";

registerHooks({
  load(url, context, nextLoad) {
    if (url.endsWith(".css")) return { format: "module", source: "", shortCircuit: true };
    if (!url.endsWith(".jsx")) return nextLoad(url, context);
    const { code } = transformSync(readFileSync(fileURLToPath(url), "utf8"), { loader: "jsx", jsx: "automatic", format: "esm" });
    return { format: "module", source: code, shortCircuit: true };
  },
});
