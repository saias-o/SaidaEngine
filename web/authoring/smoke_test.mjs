// Smoke test for the headless authoring-validation WASM (Phase C2/C3).
// Loads the module in-process (as the Node Collaboration Gateway will) and
// checks that the real C++ validateOpShape/parseSaidaOp run through WASM.
//
//   node web/authoring/smoke_test.mjs   (after ./web/build_authoring_wasm.sh)

import { fileURLToPath, pathToFileURL } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const modPath = resolve(here, "../../build-authoring-wasm/saida_authoring.mjs");

const factory = (await import(pathToFileURL(modPath).href)).default;
const Module = await factory();

const validateOp = (op) =>
  JSON.parse(Module.ccall("saida_validate_op", "string", ["string"], [JSON.stringify(op)]));
const knownOpTypes = () =>
  JSON.parse(Module.ccall("saida_known_op_types", "string", [], []));
const opVersion = () => Module.ccall("saida_op_version", "number", [], []);

let failures = 0;
function check(label, cond) {
  if (!cond) {
    console.error(`FAIL: ${label}`);
    failures++;
  } else {
    console.log(`ok: ${label}`);
  }
}

// Contract surface.
check("op version is 2", opVersion() === 2);
const types = knownOpTypes();
check("known op types include set_transform", types.includes("set_transform"));
check("known op types include set_property", types.includes("set_property"));

// Valid ops.
check(
  "valid set_transform accepted",
  validateOp({ type: "set_transform", payload: { nodeId: "2", position: [1, 2, 3] } }).ok === true
);
check(
  "valid set_property accepted",
  validateOp({ type: "set_property", payload: { nodeId: "2", property: "intensity", value: 2 } }).ok === true
);

// Invalid ops — the same rejections the native engine gives.
const unknownType = validateOp({ type: "explode_scene", payload: {} });
check("unknown op type rejected", unknownType.ok === false && /unknown op type/.test(unknownType.error));

const missingField = validateOp({ type: "set_transform", payload: { nodeId: "2" } });
check("set_transform without a channel rejected", missingField.ok === false);

const badVersion = validateOp({ type: "delete_node", opVersion: 999, payload: { nodeId: "2" } });
check("unsupported opVersion rejected", badVersion.ok === false);

const notJson = JSON.parse(Module.ccall("saida_validate_op", "string", ["string"], ["not json at all"]));
check("malformed JSON reported, not thrown", notJson.ok === false);

if (failures > 0) {
  console.error(`\n${failures} check(s) failed`);
  process.exit(1);
}
console.log("\nall authoring-wasm checks passed");
