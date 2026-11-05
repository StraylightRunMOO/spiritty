// Type declarations for the Embind-generated spiritty.mjs module.
//
// This is a thin re-export so that dynamic `import("./spiritty.mjs")` calls
// resolve types (TypeScript 5.7+ under moduleResolution: "Bundler" maps
// `.mjs` imports to `.d.mts`, not `.d.ts`). The real surface lives in
// spiritty.d.ts, which is loaded by the top-level `import ... from
// "./spiritty.js"` and kept in sync with the C ABI by hand.
export * from "./spiritty.js";
export { default } from "./spiritty.js";
