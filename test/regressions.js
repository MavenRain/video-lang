// End-to-end regressions for the checker and JSON target, including custom domains.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

const kit = path.resolve(process.argv[2] || '.');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'langc-regressions-'));
const compiler = path.join(kit, 'build/langc');
let sequence = 0;
let failed = 0;
let checked = 0;

function run(exe, args) {
  const r = spawnSync(exe, args, {cwd: kit, encoding: 'utf8'});
  assert.ifError(r.error);
  assert.equal(r.signal, null, `${exe} terminated by ${r.signal}`);
  return r;
}

function source(text) {
  const file = path.join(tmp, `source-${sequence++}.lang`);
  fs.writeFileSync(file, text);
  return file;
}

function built(text, exe = compiler) {
  const r = run(exe, ['build', source(text)]);
  assert.equal(r.status, 0, r.stderr);
  assert.equal(r.stderr, '');
  assert(!r.stdout.includes('\ufffd'), 'output is not UTF-8');
  return JSON.parse(r.stdout);
}

function refused(text, code, exe = compiler) {
  const file = source(text);
  const out = path.join(tmp, `output-${sequence++}.json`);
  for (const args of [['build', file], ['build', file, '-o', out]]) {
    const r = run(exe, args);
    assert.equal(r.status, 1, r.stdout + r.stderr);
    assert(r.stderr.startsWith(`langc: ${code}: `), r.stderr);
    assert.equal(r.stdout, '', 'refusal wrote a partial document');
    assert(!fs.existsSync(out), 'refusal created an output file');
  }
}

function domainCompiler(text) {
  const domain = source(text);
  const embedded = path.join(tmp, `domain-${sequence++}.c`);
  const exe = path.join(tmp, `langc-${sequence++}`);
  // Like the Makefile, TCC can name a compiler plus driver flags.
  function tcc(args) {
    const r = spawnSync('sh', ['-c', 'exec $TCC "$@"', 'tcc', ...args], {
      cwd: kit, encoding: 'utf8', env: {...process.env, TCC: process.env.TCC || 'tcc'},
    });
    assert.ifError(r.error);
    assert.equal(r.status, 0, r.stdout + r.stderr);
  }
  tcc(['-run', 'gen/embed.c', domain, embedded]);
  const front = ['base', 'lexer', 'parser', 'front', 'eval', 'check'].map(n => `src/front/${n}.c`);
  // The media back end links the libav* of the FFmpeg pin (the Makefile passes its flags).
  const words = name => (process.env[name] || '').split(/\s+/).filter(Boolean);
  tcc(['-std=c99', '-Wall', '-Werror', '-Isrc', ...words('FFMPEG_CFLAGS'), '-o', exe, 'src/main.c', ...front,
    'src/json.c', 'src/media.c', embedded, ...words('FFMPEG_LDLIBS')]);
  return exe;
}

function test(name, fn) {
  checked++;
  try { fn(); }
  catch (error) { failed++; console.error(`FAIL ${name}: ${error.message}`); }
}

try {
  test('Sum bind preserves its error type', () => {
    refused('def s : Sum Flag Nat := inl flagNo\ndef bad : Sum Nat Nat := bind s (fun (n : Nat) => inr n)', 'TYPE_MISMATCH');
    refused('def s : Sum (Eq Nat 0 0) Nat := inl refl\ndef bad : Sum (Eq Nat 0 1) Nat := bind s (fun (n : Nat) => inr n)', 'TYPE_MISMATCH');
    refused('def s : Sum (Type 1) Nat := inl (Type 0)\ndef bad : Sum (Type 0) Nat := bind s (fun (n : Nat) => inr n)', 'TYPE_MISMATCH');
    const doc = built('def s : Sum Flag Nat := inl flagNo\ndef keep : Sum Flag Unit := bind s (fun (n : Nat) => inr unit)\ndef success : Sum Flag Nat := bind (inr 3) (fun (n : Nat) => inr (natAdd n 1))');
    assert.deepEqual(doc.instances[1].value, {inl: false});
    assert.deepEqual(doc.instances[2].value, {inr: 4});
  });

  test('printed types preserve binding under normalization', () => {
    const cases = [
      ['def T : Nat -> Type 0 := fun (n : Nat) => Sigma (x : Nat) (Lot n)\n', 'Sigma (x : Nat) (T x)', 'pack 3 (pack 4 (makeLot 3 9))'],
      ['def T : Type 0 := Nat\n', 'Sigma (Nat : Nat) T', 'pack 3 4'],
      ['def _x0 : Type 0 := Nat\ndef T : Nat -> Type 0 := fun (n : Nat) => Sigma (x : Nat) (Lot n)\n', 'Sigma (x : Nat) (T x)', 'pack 3 (pack 4 (makeLot 3 9))'],
      ['def plus : Nat -> Nat := natAdd 1\n', 'Option (Eq (Nat -> Nat) plus plus)', 'some refl'],
    ];
    for (const [prefix, type, value] of cases) {
      const original = built(`${prefix}def holder : ${type} := ${value}`).instances[0];
      const replay = built(`${prefix}def replay : ${original.type} := ${value}`).instances[0];
      assert.deepEqual(replay.value, original.value);
    }
  });

  test('more than 64 dependent binders print safely', () => {
    const n = 66;
    const type = Array.from({length: n}, (_, i) => `Sigma (n${i} : Nat) `).join('') + `(Eq Nat n${n - 1} n${n - 1})`;
    const value = 'pack 0 ('.repeat(n) + 'refl' + ')'.repeat(n);
    const instance = built(`def deep : ${type} := ${value}`).instances[0];
    assert(instance.type.includes(`Eq Nat n${n - 1} n${n - 1}`), instance.type);
    built(`def replay : ${instance.type} := ${value}`);
  });

  test('overlong type metadata is refused without partial output', () => {
    let text = 'def T0 : Type 0 := Nat\n';
    for (let i = 1; i <= 10; i++) text += `def T${i} : Type 0 := Prod T${i - 1} T${i - 1}\n`;
    refused(text + 'def empty : Option T10 := none', 'JSON_TYPE');
  });

  test('domain fields cannot duplicate JSON keys or constructor names', () => {
    for (const domain of ['family R := makeR (f : Nat) (f : Nat)', 'family R := makeR (makeR : Nat)']) {
      refused('', 'REFUSE_NAME', domainCompiler(domain));
    }
    refused('', 'DOMAIN_FAMILY', domainCompiler('family R := makeR (tag : Nat) | stopR'));
    const single = built('def r : R := makeR 7', domainCompiler('family R := makeR (tag : Nat)'));
    assert.deepEqual(single.instances[0].value, {tag: 7});
  });

  test('families live in the universe of their fields', () => {
    const exe = domainCompiler('family U := makeU (decode : Type 0)\nfamily W := makeW (wrapped : U)\nfamily Box (A : Type 1) := box (unbox : A)');
    for (const family of ['U', 'W', 'Box (Type 0)']) refused(`def small : Type 0 := ${family}`, 'TYPE_MISMATCH', exe);
    const accepted = run(exe, ['check', source('def large : Type 1 := U\ndef nested : Type 1 := W\ndef u : U := makeU Nat\ndef b : Box (Type 0) := box (Type 0) Nat')]);
    assert.equal(accepted.status, 0, accepted.stderr);
  });
} finally {
  fs.rmSync(tmp, {recursive: true, force: true});
}
console.log(`regressions: ${checked} checked, ${failed} failed`);
process.exitCode = failed === 0 ? 0 : 1;
