# Glidulator GL-1 Interoperability Notes

*ElTentakel Graphics Systems, Publication No. ETGS-GL1-IN*

For developers of DOS extenders, DPMI hosts, memory managers, sound
emulators and BIOS layers: what the DOS module of Glidulator does on the
machine, what it relies on, and what would break it. Written for SBEMU,
VDPMI, HDPMI, JEMM and CSMWrap in particular, but valid for any host.

Glidulator emulates a 3dfx Voodoo Graphics in software. Under DOS it ships
as `GLIDE2X.OVL`, a DOS/4G LE DLL that games load in place of 3dfx's own.
It renders on several CPU cores. DOS has no scheduler, so the OVL starts
the other cores itself while the game runs. That is the part that touches
your project.

## 1. What the OVL does on the machine

### Main core (the game's core)

- Runs in whatever ring the host gives the game. No TSR, no driver, nothing
  stays resident after the game exits.
- Uses x87, MMX and, when it is usable, SSE/SSE2 (the JIT renderer).
- Calls the video BIOS through DPMI 0300h (VBE), maps the linear
  framebuffer through DPMI 0800h, and allocates memory through 0501h/0502h.
- Polls ports 3DAh (retrace) and writes 3C8h/3C9h (palette). In ring 3
  these may trap through the host's I/O bitmap; that is slower but fine.
- **In ring 0 only** (DOS/4GW without a DPMI host):
  - sets `CR4.OSFXSR` if the BIOS has not, and restores it at
    `grSstWinClose`;
  - adds a write-combining MTRR range for the framebuffer (Intel's MTRR
    update procedure, interrupts off), and restores the original MTRRs at
    close. Set `GLIDULATOR_WC=0` to disable.

### Other cores (APs)

- **Which cores**: only the enabled local APICs in the ACPI MADT, one
  logical CPU per physical core, never the main core's own core or its
  Hyper-Threading sibling. If there is an MADT, the OVL never broadcasts an
  IPI. CPUID leaf 0Bh topology and a broadcast (all excluding self) are the
  fallbacks only when there is no MADT.
- **Start**: INIT, 10 ms, two Startup IPIs to a 4 KB trampoline in DOS
  memory (allocated with DPMI 0100h, freed afterwards).
- **While running**: ring 0, interrupts off (`cli`), own IDT for exceptions
  only. No port I/O, no PIC or IRQ changes, no BIOS calls, no DPMI calls,
  no MSR writes. The APs read and write the OVL's own memory (render queue,
  framebuffer copy, texture memory, JIT code).
- **Lifetime**: only while Glide is open. Unused APs halt after 200 ms.
  When the last worker ends (at `grSstWinClose`/`grGlideShutdown`), every
  AP receives INIT and is back in wait-for-SIPI, as the BIOS left it.
- **Guard**: before an AP touches any memory outside the trampoline page,
  it compares a heap word and the first code bytes of its entry point with
  the main core's view. On a mismatch it halts, and the OVL continues on one
  core. A wrong address space therefore leads to "single core", never to
  stray writes.

`GLIDULATOR_SMP=off` (in `GLIDULAT.CFG` or the environment) turns all of
this off. `GLIDULATOR_THREADS=1` keeps the cores asleep as well.

## 2. Environments and the path the OVL takes

| Environment | Ring of the game | Multi-core path | SSE/JIT on the main core |
|---|---|---|---|
| DOS/4GW, no memory manager, or HIMEM/HIMEMX only | 0 | **ring0**: APs use DOS/4GW's GDT, no paging | yes (OVL sets `CR4.OSFXSR`) |
| EMM386 or another VCPI server without DPMI | 0, paging | none | yes |
| HDPMI32i (SBEMU with JEMMEX/QPIEMU) | 3 | **paging**: APs load the host's page directory | yes (HDPMI sets `CR4.OSFXSR`) |
| VDPMI (SBEMU pre-release) | 3 | **flat**: the host maps 1:1, APs run without paging | no (C path on the main core) |
| Windows 3.1/9x DOS box | 3 | none (detected, never attempted) | Windows 98SE: yes (it sets `CR4.OSFXSR`) |

`GLIDULATOR_SMP=auto|ring0|paging|flat|off` selects a path explicitly;
`auto` is the default. A forced path that does not fit the host is refused
and the OVL stays on one core.

## 3. What the ring-3 paths rely on

SBEMU traps the Sound Blaster ports through the I/O permission bitmap,
which works only above IOPL. The game, and with it the OVL, therefore runs
in ring 3. The APs do not belong to the host, so they can still run in
ring 0. To do so, the OVL needs the following from the host. Please keep
these behaviours, or tell us before you change them.

### All DPMI hosts

- **Flat, zero-based selectors** for the client: CS and DS with base 0 and
  limit 4 GB (checked with DPMI 0006h and `LSL`).
- **DPMI 0202h/0203h**: exception handlers for #UD (6), #GP (13) and #PF
  (14). The OVL installs them only around a few probe instructions at
  start-up and restores the previous handlers right away. A privileged
  instruction the host does not emulate must reach the client's handler
  instead of terminating the program.
- **DPMI 0800h** for the local APIC (`FEE00000h`, xAPIC mode) and for the
  ACPI tables (RSDT/XSDT, MADT). Mapping the same physical page twice may
  return the same linear page (HDPMI does); the OVL keeps only one mapping
  at a time and frees it with **0801h**.
- **DPMI 0100h/0101h** for the trampoline. The first megabyte must be
  identity-mapped (linear = physical): the trampoline page, the BIOS data
  area (`40:0E`, EBDA) and `E0000h-FFFFFh` (RSDP search).
- **xAPIC mode**. With x2APIC enabled, the ring-3 paths stay off.
- **Windows detection**: INT 2Fh AX=160Ah returning AX=0 and CX=3
  (enhanced mode) means Windows; the OVL then never starts cores. A host
  that answers INT 2Fh AX=1600h like Windows for compatibility is fine, as
  long as 160Ah does not report CX=3. VDPMI currently answers 1600h with
  AL=4 and 160Ah with CX=2, which works.
- **Interrupt handlers** of the host and of TSRs such as SBEMU run on the
  main core between game instructions. They must save any FPU/MMX/SSE
  state they touch. SBEMU is built for i386 and does not use SSE in its
  interrupt path, which is what makes the JIT safe under HDPMI.

### Paging hosts (HDPMI32i)

- **Reading CR0, CR3 and CR4 from ring 3 is emulated** and returns the real
  values. The APs load exactly this CR0, CR3 and CR4.
- **32-bit paging without PAE**, 4 KB or 4 MB pages. `CR4.PAE` set means
  "not supported".
- **One page directory for the client** for the whole session. Pages that
  the OVL's workers use must not move to another physical address while
  Glide is open. New mappings are fine. Each AP reloads CR3 when it starts
  its job, and not-present entries are never cached.
- `CR4.OSFXSR` set by the host means the host saves SSE state. The OVL then
  uses the JIT on the main core.

### 1:1 hosts (VDPMI)

- **Every linear address the host gives the client equals the physical
  address**: DPMI memory, DOS memory, and the result of DPMI 0800h. The APs
  run without paging and see the same memory.
- `mov eax, cr3` from ring 3 returns 0 (or raises #GP), and DPMI 0800h
  returns the physical address unchanged. That is how the OVL recognises
  this kind of host.
- If a future version maps client memory differently, for example with
  `/EMS`, please emulate reading CR0/CR3/CR4 the way HDPMI does. The OVL
  will then use its paging path automatically. Setting `CR4.OSFXSR` would
  enable the JIT on the main core. Keeping the emulated `RDMSR 1Bh`
  (APIC base, x2APIC flag) helps as well.

## 4. BIOS layers (CSMWrap and native CSMs)

- **Hide reserved CPUs in the MADT.** CSMWrap keeps one AP as its BIOS
  proxy and removes it from the MADT and the MP table. The OVL only wakes
  CPUs the MADT lists as enabled, so it leaves the proxy alone. It was
  tested with JEMMEX in V86 mode, where disk I/O goes through the proxy,
  and the disk still worked after a multi-core session. A layer that keeps
  a CPU for itself but lists it as enabled would lose it to an INIT.
- **Low extended memory.** DOS/4GW without XMS (the most compatible setup
  for DOS/4GW games) knows only the block INT 15h E801/88h reports, from
  1 MB up to the first reserved range. UEFI memory maps can reserve memory
  low down (OVMF: ACPI NVS at 8 MB), which leaves DOS/4GW a few MB, and
  `DOS16M=@...` cannot work around it. HIMEMX and JEMMEX read E820 and are
  not affected. The stick's `MEMCHK.COM` detects the case and recommends
  them.
- **ACPI tables** must be reachable below 4 GB through the RSDT or through
  an XSDT entry with a zero upper half.

## 5. Checking a host: `R3PROBE.EXE`

`tools/dosvm/r3probe.c` (built by `tools/dosvm/run.sh`) is a small DOS/4GW program that shows what a DPMI host
offers a ring-3 client, and then does what the OVL does:

```
R3PROBE        read-only checks
R3PROBE AP     additionally start the other cores (as the OVL does)
```

It appends to `R3PROBE.TXT` and closes the file after every line, so a
hang still leaves the output up to that point. It reports:

- CPL, selector bases and limits, DPMI version;
- INT 2Fh 1600h/160Ah/1683h;
- `mov eax, cr0/cr3/cr4`, a CR4 write of the same value, `rdmsr 1Bh`,
  `xorps` (SSE usable?);
- whether the GDT is readable;
- a page walk from CR3 that checks static data, stack, heap, code and DOS
  memory against a second mapping of the physical page;
- the local APIC through DPMI 0800h and the MADT.

With `AP`, it starts the cores (MADT targets) with an own flat ring-0 GDT
and the host's CR3, or without paging. Each AP sums a heap array with
program code, writes to the heap and halts. INIT then returns it to
wait-for-SIPI. A correct run ends like this:

```
AP: 3 started, stopped by the guards: 0 (trampoline) 0 (probe), exceptions 0
  AP 0: APIC 1, done 1, sum OK, heap write OK
```

Results so far (QEMU/KVM, 4 cores):

| Host | CR0/CR3/CR4 from ring 3 | Paging | SSE in ring 3 | APs |
|---|---|---|---|---|
| HDPMI32i (SBEMU 1.0.0-beta.6) | emulated, real values | on, no PAE | yes | 3/3 |
| VDPMI 0.20 (SBEMU pre-release) | CR0/CR4 #GP, CR3 reads 0 | 1:1 | #UD | 3/3 |
| DOS/4GW, no host | native | off | yes | 3/3 |
| Windows 98SE DOS box | read as 0, no fault | (not walked) | yes | none: CPUID reports no APIC, INT 2Fh 160Ah CX=3 |
| any of the above under CSMWrap 3.1.2 (OVMF) | as above | as above | as above | 2/2 (one CPU kept by CSMWrap) |

## 6. Known issues involving other projects

| Observed | Where | Status |
|---|---|---|
| SBEMU (VDPMI build) raises a General Protection Fault while loading, at `CS:EIP` = `F000:761D` (BIOS code, probably the 32-bit PCI BIOS service), SS base `B5D14000` | Asus laptop, Ryzen 5 5500U, booted through CSMWrap | Before Glidulator is involved; not reproducible in QEMU with CSMWrap (AC97). Open. |
| No sound under SBEMU/HDPMI32i (Nature, GTA); Dimension hangs on a black screen with sound | same laptop | Not yet separated: card detection, speaker path, or Glidulator. Open. |
| No audible sound under SBEMU/HDPMI32i; Nature and Dimension run with 3 render workers and the JIT | Core i7-4770T (Intel HDA), BIOS boot | The machine stays silent with the native Windows HDA driver as well, so most likely the codec's output routing. Dimension does not hang here. |
| Dimension hangs | VDPMI pre-release, same i7-4770T | Open; VDPMI is experimental. |
| Nature stays black after its countdown | VDPMI, also on one core | Open; seen in QEMU only. |
| WAV recording of QEMU's AC97 stays silent under SBEMU | QEMU | Audible sound needs real hardware. |
| DOS/4GW without XMS reports "not enough memory" | UEFI memory maps with a low reserved range (CSMWrap on OVMF) | Use HIMEMX or JEMMEX (section 4). |

## 7. Testing without hardware

- `tools/dosvm/run.sh <MB> none|himemx|sbemu|vdpmi` runs FreeDOS in QEMU
  with the chosen setup (`sbemu` = JEMMEX + QPIEMU + HDPMI32i + SBEMU,
  `vdpmi` = HIMEMX + VDPMI + SBEMU, AC97 card).
  - `R3PROBE=AP NATUREONLY=0` runs the probe.
  - `OVLTEST=1` runs a two-triangle test through the real OVL.
  - VDPMI needs VME, so use KVM: `QEMU_OPTS="-enable-kvm -cpu host -smp 4"`.
- The debug OVL accepts `GLIDULATOR_SMP_TEST=flat|magic` to check the
  guard on your host:
  - `flat` runs the APs in a wrong address space under a paging host;
  - `magic` corrupts the expected value.

  Expected result: one core, no damage.

## 8. Contact

Findings, logs and photos of crash screens are welcome as issues at
<https://github.com/ElTentakel/Glidulator/issues>. The trace OVL
(`BIN\TRACE\` in the release package) writes `GLIDULAT.LOG` next to the
game, with lines starting with `Multi-core:` that show the path the OVL
took and why.
