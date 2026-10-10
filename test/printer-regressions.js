// Exercise the printer past its former 64-name boundary through both commands.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "langc-printer-"));
try {
  const names = Array.from({ length: 65 }, (_, i) => `n${i}`);
  const source = path.join(tmp, "deep.lang");
  const functionType = names.map(n => `(${n} : Nat) -> `).join("") + "Nat";
  const functionBody = names.map(n => `fun ${n} => `).join("") + "n64";
  const sigmaType = names.map(n => `Sigma (${n} : Nat) `).join("") + "(Eq Nat n64 n64)";
  const sigmaBody = names.reduce(body => `pack 0 (${body})`, "refl");
  fs.writeFileSync(source, `def deep : ${functionType} := ${functionBody}\n` +
    `def nested : ${sigmaType} := ${sigmaBody}\n`);
  const run = (...args) => {
    const result = spawnSync("build/langc", args, { encoding: "utf8" });
    assert.equal(result.status, 0, `${args[0]}: ${result.signal || result.error || result.stderr}`);
    assert.equal(result.stderr, "");
    return result.stdout;
  };
  assert.equal(run("eval", source, "deep"), functionBody + "\n");
  const doc = JSON.parse(run("build", source));
  assert.equal(doc.instances.length, 1);
  assert.match(doc.instances[0].type, /Eq Nat n64 n64/);
  let value = doc.instances[0].value;
  for (const name of names) {
    assert.equal(value.witness, 0, name);
    value = value.payload;
  }
  assert.equal(value, null);
  console.log("printer regressions: 2 checked");
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
