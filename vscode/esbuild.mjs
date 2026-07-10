import * as esbuild from "esbuild";

const watch = process.argv.includes("--watch");

/** @type {import("esbuild").BuildOptions} */
const common = {
  bundle: true,
  format: "cjs",
  platform: "node",
  target: "node18",
  sourcemap: true,
  logLevel: "info",
};

/** @type {import("esbuild").BuildOptions[]} */
const builds = [
  // The extension entry point runs inside the VS Code extension host.
  {
    ...common,
    entryPoints: ["src/extension.ts"],
    outfile: "dist/extension.js",
    external: ["vscode"],
  },
  // The language server runs as a separate node process (IPC transport).
  {
    ...common,
    entryPoints: ["src/server/server.ts"],
    outfile: "dist/server.js",
  },
  // The debug adapter is launched by VS Code as a standalone node program.
  {
    ...common,
    entryPoints: ["src/debugAdapter.ts"],
    outfile: "dist/debugAdapter.js",
  },
  // Standalone CJS bundles of the protocol and type database modules so the
  // smoke test (scripts/protocol-smoke.mjs) can exercise them with plain node.
  {
    ...common,
    entryPoints: ["src/common/protocol.ts"],
    outfile: "dist/protocol.cjs",
  },
  {
    ...common,
    entryPoints: ["src/server/typedb.ts"],
    outfile: "dist/typedb.cjs",
  },
];

if (watch) {
  const contexts = await Promise.all(builds.map((options) => esbuild.context(options)));
  await Promise.all(contexts.map((ctx) => ctx.watch()));
  console.log("[esbuild] watching...");
} else {
  await Promise.all(builds.map((options) => esbuild.build(options)));
}
