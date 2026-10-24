# Working on fajita (agents and humans)

This repository is the OnePlus 6T board. The branch `ios16-port` is **the** tree: the iOS 16.7.16 port with the GPU, touch, PMIC
(charging, keys, battery), USB debug channel, RTC, panel/haptics/alert slider and app install work merged in. Everything new lands
here. Older branches (`main`, `fajita-linux`, the iPadOS 17 work) are history.

## Build and boot the best build

```
scripts/fajita/build.sh          # kernel + GPU plugin + payload + boot image -> .cache/fajita-out/
scripts/fajita/boot.sh           # phone in fastboot: flash system_a, arm slot b, fastboot boot
```

`build.sh --test` makes a build that resets itself (for unattended runs); without it the phone stays up. Extra arguments are boot
arguments (`scripts/fajita/build.sh fajita-brightness=1`). The UniXNU stack is a worktree of `~/src/UniXNU` at the commit in
`ios16/gpu-plugin.ref` (default `~/src/UniXNU-fajita-stack`); `build.sh` creates it and moves it there. To change the stack, commit to
the UniXNU fork (`johnpanos/UniXNU`, branch `fajita-a630-userland`), push, and bump `ios16/gpu-plugin.ref` in the same commit as any
board change that needs it.

### Inputs (not in git)

`.cache/` is gitignored and holds what cannot be committed: the Apple SDK (`apple-sdk/`), the Metal compiler under Wine
(`metal-win/`), GPU plugin build state (`gpu-local/`), trust caches (`trust/`), the data seed (`data-seed-ios16/`), the verified
root (`verified-root/`), iig outputs (`iig-outputs-20H392/`), firmware (`ios16/.cache/firmware/`) and `fajita-inputs/` (`boot.img`,
the DarwinBoot boot image; `storage-candidate/` (kernel.macho + kernel.json); `metal-base.ngpu`, the plugin add-on base; `platform-seed`; `fjdbg-agent`).

## Testing on the phone

Tools are in `scripts/fajita/test/`; their state (logs, `phone.lock`, reference screenshots) is in `.cache/fajita-work/`
(`FAJITA_WORK`).

| Tool | Use |
| --- | --- |
| `session.sh start` / `run CMD` / `push` / `pull` / `call OP` / `end NAME` | boot once, then run many tests over USB without rebooting |
| `cycle.sh NAME [PAYLOAD BOOT]` | one unattended run: flash, boot, stream the log, wait for the reset |
| `gpu-status.sh` | one-shot health of the current run: holder, queue, log age, GPU stage, panics, crashes |
| `fps.sh` | the standard frame-rate benchmark: home page 1 <-> 2 swipes, then App Library scroll |
| `gpu-ab.sh ENV...` | A/B plugin settings in one session: frame rate, GPU busy, screenshot diff per variant |
| `settings-scroll.sh NAME` | correctness pass: open Settings, scroll, screenshot each step |
| `perf.py`, `passes.py`, `ts.py` | GPU counters, per-pass timing, timestamps |
| `hc OP ...` | a raw hub call (`run`, `kcmd`, `screenshot`, `read32`, `reset`, ...) |

## Best practices

**One tree.**
- Work on `ios16-port`, or a short-lived branch of it.
- Merge the latest `origin/ios16-port` before every build. Push when a change works; unpushed work is invisible to everyone else.
- Use real git worktrees (`git worktree add`), never copied directories. A copy shares or loses the gitdir and its submodule state.
- Commit explicit paths (`git add PATH`), never `git commit -a`. In trees with submodules, `-a` silently reverts submodule pins
  (NativeAdreno, NativeMetal, metal2vulkan).
- Bump a submodule pin in the same commit as the code that needs it.
- Push submodule and stack changes to the `johnpanos` forks. Ask before pushing to shared upstream repos (UniXNU org, DarwinBoot).

**The phone is shared.**
- Build first, then take the phone. `cycle.sh` and `session.sh` hold `phone.lock`, so runs queue rather than collide.
- Keep sessions short. An idle session ends after 300 s. Past 1200 s, a session ends when another run is waiting.
- End only your own session (`session.sh end NAME`).
- One hub per tty. Talk to the phone through `hc`, `session.sh` or `fajita-usbdbg-hub.py call`; never open the tty or start a hub
  yourself while a run is going. A second reader splits the stream, and replies are lost.

**Unattended and self-recovering.**
- Nobody will press buttons on the phone. Test builds use `--test` (autoreset, plus the hardware watchdog stop).
- A hang must end in fastboot on its own.
- iOS is the default. Boot pmOS only when the USB capture failed; it is not used for charging, because XNU charges the battery.
- Touch, charging, keys and battery are on in every build (`board_args`). Don't turn them off.

**No long blind waits.**
- Stream logs. Poll with short checks such as `gpu-status.sh`, or a tail of `usb-console.txt`.
- Never block for minutes on one command without output. Run long jobs in the background and check them.
- A run that stops producing log output is broken. Stop it instead of waiting.

**Measure the same way every time.**
- Speed is `fps.sh`: home swipes and App Library scroll. Report both numbers.
- Correctness is a screenshot diff against the references (`settings-scroll.sh`, `gpu-ab.sh`). A faster frame that draws wrong is
  a regression.
- Check your own diagnostics first: debug stops, `boot-progress` and probe arguments in the kit can be the cause of a "hang".

**Never commit blobs.**
- Apple material (IPSW contents, kernelcaches, the SDK, frameworks, trust caches, screenshots of the iOS UI) and Qualcomm firmware
  stay in `.cache/`.
- No keys in git. The SDK export key lives in `~/.config/unixnu/`.
