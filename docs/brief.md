# The owner's briefs

`todo.txt` was rewritten on 2026-08-26 as a list of open items only.  What it
held before that was two briefs, one after the other, in the owner's own words.
Both are reproduced here unedited, line for line, because they are the only
record of why emu88 exists and of what "done" was defined to mean; neither
survives anywhere else outside `git log`.

## Second brief - emu88, added 2026-06-04 in f2392f0

This replaced the first brief wholesale when the validation suites landed.  It
is the one that was still at the top of `todo.txt` up to 7352fc5.

```
A previous project was an 8080 emulator (see ../cpmemu).
on top of the instructions the CP/M os calls were emulated.
See the ../copmemu .cfg system for mapping linux files into
a 8.3 file space.

We started doing the same thing here.  You created emu88 as an 8088 emulator.
you later extended to 386.  But when I asked to extend it to add protected mode
you balked and suggested using dosobox as they spent over 2 years tweaking
protected mode to work right.

Id like to back off the dosbox part. Dont remove it --yet.  But focus on the emu88 emultator.
Get it working 100% for 386 all modes.  I suggest finding test suites / instruction exercisors
for testing.
```

Nothing in it is closed.  "Get it working 100% for 386 all modes" is still the
goal - the suites that now measure it are real-mode-per-instruction plus one
full-system ROM pass, and what they do not reach is in `todo.txt`.  Backing off
dosbox is partly done and partly not: emu88 is the default backend now
(`qxDOS/Views/MachineConfig.swift`, `var backend: EmulatorBackend = .emu88`, set
in 932af28), and `dosbox-staging` is still a submodule, still selectable, and
still what `README.md` and `CLAUDE.md` describe as the engine.

## First brief - selectable backend and DOS layer, added 2026-04-10 in fa94af4

The whole of `todo.txt` as it was first committed.  Every line of it is done:
073605d made the hardware backend selectable, 8cec5b7 made the DOS layer
selectable, and `MachineConfig.swift` captures the backend for the run at start
so it cannot be changed mid-run.

```
todo.txt

make this dos emulator more configurable
for the hardware level make the emulator selectable 
from ../iosFreeDOS and the current dosbox in this repo.

For the DOS level support https://freedos.org/
real msdos
and the dosbox built in dos

On the settings screen the above can be set.
Once set and started it cant be changed that run
(we never got reuse/restart of an emulator to work)
```

The parenthesis on the last line was answered for one backend only.  073605d's
commit message records that emu88 "supports clean in-process restart, so its
stop() does a real teardown instead of _exit(0); only the DOSBox path still
terminates the process".  That is the commit's own claim and it was not
re-checked against the bridge when this file was written.
