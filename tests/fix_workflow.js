export const meta = {
  name: 'emu88-386-fix',
  description: 'Parallel fix of emu88 386 instruction bugs against SingleStepTests/80386',
  phases: [{ title: 'Fix', detail: 'one agent per instruction family' }],
}

const MAIN = '/Users/wohl/src/qxDOS'
const DATA = `${MAIN}/tests/data/80386/v1_ex_real_mode`
const REVOKE = `${MAIN}/tests/data/80386/revocation_list.txt`

const PREAMBLE = `
You are fixing bugs in the emu88 80386 CPU emulator (a standalone C++ interpreter)
so that it passes the SingleStepTests/80386 real-mode instruction test suite.

## Setup (do this first)
The authoritative source lives at ${MAIN}/emu88/ . Work on a PRIVATE COPY so you
do not collide with other agents:

    WORK=/tmp/emu88fix_<PICK_A_UNIQUE_NAME>
    mkdir -p "$WORK"
    cp ${MAIN}/emu88/*.cc ${MAIN}/emu88/*.h "$WORK"/
    cp -r "$WORK" "$WORK.orig"     # pristine backup for producing final hunks

Build a private test binary (matches tests/build.sh, but compiles YOUR copy):

    build() {
      clang++ -std=c++20 -O2 -g -I "$WORK" -I ${MAIN}/tests/vendor -DMOO_USE_ZLIB \\
        ${MAIN}/tests/sst386.cc \\
        "$WORK/emu88.cc" "$WORK/emu88_pmode.cc" "$WORK/emu88_fpu.cc" "$WORK/emu88_mem.cc" \\
        -lz -o /tmp/sst_<UNIQUE> 2>&1 | grep -iE 'error' ; echo "build done"
    }
    build

Run the suite on your opcodes (the prebuilt shared binary also exists at
${MAIN}/tests/build/sst386 for read-only baselining):

    # summary for one opcode file (prefix match on basename):
    /tmp/sst_<UNIQUE> --only 0FBC --summary --revoke ${REVOKE} ${DATA}
    # rich per-failure dump (instruction bytes + initial regs/EFLAGS + expected EFLAGS):
    /tmp/sst_<UNIQUE> --only 0FBC --show 8 --diag --revoke ${REVOKE} ${DATA}
    # whole suite (≈8s) then grep your opcodes:
    /tmp/sst_<UNIQUE> --summary --revoke ${REVOKE} ${DATA} | grep -E '<your-op-regex>'

## How the test works
Each test loads an initial CPU state, runs ONE instruction (the suite appends a
HLT), and compares the final registers + RAM. Real mode; 16MB RAM; A20 on.
EFLAGS bit layout: CF=0x1, (bit1 always 1)=0x2, PF=0x4, AF=0x10, ZF=0x40,
SF=0x80, TF=0x100, IF=0x200, DF=0x400, OF=0x800.
The harness already applies each file's official undefined-flag mask (RM32 chunk).
So ANY remaining EFLAGS mismatch the harness reports is a REAL required value you
must reproduce — either a defined flag, or a flag the 386 sets deterministically
even though the manual calls it "undefined". The 80386 in this suite is a 386EX;
its deterministic "undefined" flag behavior must be matched EXACTLY (no guessing —
derive empirically from the --diag dumps across many tests, and/or consult how
mature emulators model 386 undefined flags; you may use web search).

## Your job
1. Baseline your opcode files; capture --diag output for a dozen+ failing tests.
2. Read the relevant handler(s) in $WORK/emu88.cc (line numbers given below).
3. Determine the exact root cause(s). Derive any "undefined-but-deterministic"
   flag rule empirically (compare init EFLAGS, operands, and expected EFLAGS across
   many tests until you find the invariant). Cross-check your hypothesis on tests
   you have NOT yet looked at.
4. Edit your private copy, rebuild, re-run, and ITERATE until your opcode files
   pass as close to 100% as achievable. ~1% residue on 67-prefixed (32-bit-address)
   variants may be a separate addressing bug outside your scope — note it and move on;
   focus on getting the NON-67 variants to 100%.
5. Keep edits LOCALIZED to your own instruction handlers. Do NOT modify shared flag
   helpers (set_flags_*), decode_modrm*, or other families' handlers — other agents
   own those. If you set "undefined" flags, do it inline in your case block.

## Output (StructuredOutput)
Return your result. CRITICAL: each hunk's old_string MUST be copied VERBATIM from
the PRISTINE file ($WORK.orig/...), exact whitespace, and be unique in the file, so
it can be applied with a literal find/replace to the authoritative source. Produce
the hunks by diffing $WORK.orig against $WORK at the end:
    diff -u "$WORK.orig/emu88.cc" "$WORK/emu88.cc"
Report file paths relative to repo root (e.g. "emu88/emu88.cc").
Report honest before/after pass counts (sum across YOUR opcode files) and any residual.
`

const SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['cluster', 'summary', 'hunks', 'beforePass', 'afterPass', 'total', 'confidence'],
  properties: {
    cluster: { type: 'string' },
    summary: { type: 'string', description: 'root cause(s) and the fix, incl. any derived 386 flag rule' },
    opcodes: { type: 'array', items: { type: 'string' } },
    hunks: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['file', 'old_string', 'new_string'],
        properties: {
          file: { type: 'string' },
          old_string: { type: 'string', description: 'verbatim pristine text, unique' },
          new_string: { type: 'string' },
          note: { type: 'string' },
        },
      },
    },
    beforePass: { type: 'integer' },
    afterPass: { type: 'integer' },
    total: { type: 'integer' },
    residual: { type: 'string', description: 'remaining failures and why' },
    confidence: { type: 'string', enum: ['high', 'medium', 'low'] },
  },
}

const CLUSTERS = [
  {
    label: 'bsf-bsr',
    body: `CLUSTER: BSF / BSR (bit scan forward/reverse).
Opcode files: 0FBC, 0FBD and their 66/660F, 670F, 67660F prefix variants.
Handlers: $WORK/emu88.cc case 0xBC (~line 4902) and case 0xBD (~line 4928), inside
the two-byte (0F) opcode switch.
These files have NO undefined-flag mask, so the 386 sets EVERY flag deterministically.
ZF and the destination register are defined (ZF=1 and dest unchanged when source==0;
else ZF=0 and dest=bit index). You must ALSO reproduce the 386's deterministic
CF/OF/SF/AF/PF behavior — derive it from --diag (note how flags change vs the initial
EFLAGS when source==0 vs source!=0).`,
  },
  {
    label: 'imul-flags',
    body: `CLUSTER: IMUL flag behavior.
Opcode files: 0FAF (IMUL r,r/m), 69 (IMUL r,r/m,imm16/32), 6B (IMUL r,r/m,imm8) and
their 66/67 prefix variants.
Handlers: $WORK/emu88.cc case 0xAF (~line 4670, in the 0F switch), case 0x69 (~line 3990),
case 0x6B (~line 4032).
CF and OF are defined (set when the full product doesn't fit the destination width).
The result (low bits) is defined. SF/ZF/AF/PF are "undefined" but the 386 sets them
deterministically — derive the rule from --diag (likely a function of the truncated
result and/or the full product). 0FAF has no mask; 69/6B have a mask — confirm with the
harness which flags still mismatch and reproduce those exactly.`,
  },
  {
    label: 'bt-family',
    body: `CLUSTER: BT / BTS / BTR / BTC (bit test & friends).
Opcode files: 0FA3, 0FAB, 0FB3, 0FBB (reg bit index) and 0FBA.4/.5/.6/.7 (imm8 bit index),
plus 66/67 prefix variants.
Handlers: $WORK/emu88.cc cases 0xA3 (~4808), 0xAB (~4824), 0xB3 (~4842), 0xBB (~4860),
0xBA (~4878) in the 0F switch.
TWO bugs to investigate:
 (a) DEFINED-result bug: for a MEMORY operand with a REGISTER bit index, the bit offset is
     NOT masked to the operand size. It selects a bit in a bit string in memory: the
     effective address advances by (bitindex DIV operandbits) units and the bit is
     (bitindex MOD operandbits); the register bit index is SIGNED. Current code wrongly
     masks the bit to 15/31 and only reads the addressed word -> wrong CF. Fix the memory
     bit addressing (and the read/written word must be the one actually containing the bit).
     For register-direct r/m and for the imm8 form, the bit index IS masked to operand size.
 (b) UNDEFINED flags: these files have no mask, so reproduce the 386's deterministic
     OF/SF/ZF/AF/PF (derive from --diag). CF is the selected bit (defined).`,
  },
  {
    label: 'shld-shrd',
    body: `CLUSTER: SHLD / SHRD (double-precision shifts).
Opcode files: 0FA4, 0FA5 (SHLD imm8/CL), 0FAC, 0FAD (SHRD imm8/CL), + 66/67 variants.
Handlers: $WORK/emu88.cc cases 0xA4 (~4692), 0xA5 (~4721), 0xAC (~4750), 0xAD (~4779).
No undefined-flag mask on most -> reproduce 386 deterministic flags exactly.
Investigate: (a) count is masked to 5 bits (& 0x1F); when count==0 NO flags change and
no store happens (current code already guards count!=0 — verify). (b) For the 16-bit form,
behavior when the masked count > 16 is special on the 386 (the result/flags are quirky —
derive from --diag). (c) CF = last bit shifted out. (d) OF defined only for count==1
(set if sign changed); AF and OF for other counts are deterministic-undefined — derive.
(e) SF/ZF/PF from the result.`,
  },
  {
    label: 'shifts-rotates',
    body: `CLUSTER: shift/rotate group (ROL ROR RCL RCR SHL SHR SAR).
Opcode files: C0, C1 (imm8 count), D0, D1 (count 1), D2, D3 (CL count), all .0-.7
extensions, + 66/67 variants.
Code: the helpers $WORK/emu88.cc do_shift8 (~762), do_shift16 (~855), do_shift32 (~945),
and the GRP2 dispatch execute_grp2_rm8/16/32 (~1038) and the C0/C1/D0-D3 case blocks in
the main switch. These files DO have undefined-flag masks (harness applies them), so the
residual mismatches are on DEFINED bits. Investigate:
 - 386 masks the shift/rotate count to 5 bits (count & 0x1F) for ALL of these. For RCL/RCR
   the rotate-through-carry is then taken modulo (width+1).
 - count==0: NO flags affected at all (CF/OF/SF/ZF/AF/PF unchanged), no result change.
 - SHL/SHR/SAR: CF = last bit shifted out; OF defined for count==1 only; SF/ZF/PF from result.
 - ROL/ROR/RCL/RCR: only CF and OF are affected (SF/ZF/AF/PF unchanged); OF defined for count==1.
 - Reproduce any deterministic AF/OF the suite still requires (derive from --diag).`,
  },
  {
    label: 'mul-div',
    body: `CLUSTER: GRP3 MUL / IMUL / DIV / IDIV (one-operand forms).
Opcode files: F6.4 (MUL8) F6.5 (IMUL8) F6.6 (DIV8) F6.7 (IDIV8); F7.4 F7.5 F7.6 F7.7
(16/32-bit), + 66/67 variants.
Handlers: $WORK/emu88.cc execute_grp3_rm8 (~1051), execute_grp3_rm16 (~1157),
execute_grp3_rm32 (~1368).
These files DO have masks. For MUL/IMUL, CF/OF are defined; SF/ZF/AF/PF are masked-undefined
but may still need deterministic values where unmasked — check the harness. For DIV/IDIV all
arithmetic flags are undefined (masked) but: a divide overflow / divide-by-zero raises #DE
(exception 0); the IP pushed on the 386 is the FAULTING instruction's IP (not next). Make
sure the divide-error detection (including signed IDIV ranges) and the exception are correct,
and reproduce any deterministic flags the suite still requires.`,
  },
  {
    label: 'bcd',
    body: `CLUSTER: BCD adjust — DAA DAS AAA AAS AAM AAD.
Opcode files: 27 (DAA), 2F (DAS), 37 (AAA), 3F (AAS), D4 (AAM), D5 (AAD).
Handlers: $WORK/emu88.cc case 0x27 (~2344), 0x2F (~2376), 0x37 (~2412), 0x3F (~2445),
0xD4 (~3513), 0xD5 (~3537).
These have masks. Reproduce the exact defined-flag behavior. Key points: DAA/DAS set CF/AF
per the BCD-correction algorithm and SF/ZF/PF from the result; OF is undefined (masked).
AAA/AAS modify AF and CF (defined) and AL/AH; SF/ZF/PF/OF are undefined (masked but may need
deterministic values - check harness). AAM (D4) divides AL by the imm8 base (default 0x0A):
imm8==0 raises #DE. AAD (D5) multiplies AH by imm8 and adds AL. Set SF/ZF/PF from AL.
Derive any deterministic undefined flags from --diag.`,
  },
  {
    label: 'stack-flag-ops',
    body: `CLUSTER: 32-bit operand stack/flag instructions.
Opcode files: 66CF (IRETD), 6660 (PUSHAD), 6661 (POPAD), 669C (PUSHFD), 669D (POPFD),
9E (SAHF), 9F (LAHF), and also check 60/61 (PUSHA/POPA 16-bit) and CF (IRET 16-bit) still pass.
Handlers: $WORK/emu88.cc case 0xCF (~3322, IRET/IRETD), 0x60 (~3859, PUSHA/PUSHAD),
0x61 (~3899, POPA/POPAD), 0x9C (~2874, PUSHF/PUSHFD), 0x9D (~2886, POPF/POPFD),
0x9E (~2924, SAHF), 0x9F (~2929, LAHF).
Root cause: these likely ignore op_size_32 and always do the 16-bit form. e.g. IRETD with
op_size_32 must pop EIP(32), CS(16, from 32-bit slot), and EFLAGS(32) — preserving the high
EFLAGS word. POPAD pops 8 dwords (skipping ESP slot). PUSHFD/POPFD push/pop 32-bit EFLAGS.
Determine from --diag exactly which bits/words are wrong (e.g. high EFLAGS word zeroed) and
make each respect op_size_32. SAHF loads SF/ZF/AF/PF/CF from AH (note which bits are forced).
Confirm 16-bit variants (no 66) still pass.`,
  },
]

phase('Fix')
const results = await parallel(CLUSTERS.map(c => () =>
  agent(PREAMBLE + '\n' + c.body, {
    label: c.label,
    phase: 'Fix',
    schema: SCHEMA,
  }).then(r => r ? { ...r, _label: c.label } : { _label: c.label, _failed: true })
))

return results
