# Vendored: Apple pam_launchd

**Upstream:** https://github.com/apple-oss-distributions/pam_modules
**Upstream tag:** `pam_modules-126` (commit `814a2ab0f6ca5e30a7d60e53a04ae9ce18e24f66`),
the OS X 10.9 release, the same era as the vendored launchd-842
**Files:** `modules/pam_launchd/pam_launchd.c` (164 lines), `modules/pam_launchd/pam_launchd.8`
**Imported:** 2026-10-21 (nextbsd/nextbsd-userland#354, PR 3)
**License:** APSL 2.0 (see `APPLE_LICENSE`); the upstream licence headers are kept

## What it is

A PAM session module. In `pam_sm_open_session`, running as root, it walks
up to the root bootstrap, asks PID 1 for the user's per-user launchd
(`bootstrap_look_up_per_user`), sets that as the process's bootstrap port
(`task_set_bootstrap_port`), and optionally moves into a named session
(`_vprocmgr_switch_to_session`, `launchd_session_type=` option or PAM env).
Everything the login then spawns inherits the per-user bootstrap.

## Status on NextBSD

Built and installed as `/usr/lib/pam_launchd.so.6` (+ `pam_launchd.so`),
the name and ABI suffix of every PAM module inherited from FreeBSD. **No
`pam.d` file references it yet**; the policy files live in
nextbsd-overlays and add it in a later PR.

## Build

- `Makefile` (bsd.lib.mk, shared object only), driven from
  `build-userland.sh` after nss_directory_services. OpenPAM headers and
  `libpam` come from the compat base in the sysroot; `liblaunch` and
  `libsystem_kernel` from earlier steps.
- `pam_launchd_probe.defs` is new. `build-userland.sh` runs the host mig on
  it to generate the client stub `pam_launchd_probeUser.c` and
  `pam_launchd_probe.h` used by the liveness probe below.

## NextBSD changes

All in `pam_launchd.c`, each marked with a `NextBSD:` comment.

| Change | Where | Why |
|---|---|---|
| Every failure returns `PAM_IGNORE` | `#ifdef __FreeBSD__` | Apple returned `PAM_SESSION_ERR` when the session switch or `_vproc_post_fork_ping` failed. On NextBSD a broken per-user session must degrade to today's behaviour (the root bootstrap), never block a login. On these paths the module also puts the process back on the root bootstrap before returning. |
| `MACH_PORT_NULL` per-user port is a failure | `#ifdef __FreeBSD__` | launchd-842's `job_mig_lookup_per_user_context` returns success with a null port if it could not create the per-user launchd. Apple would set a null bootstrap port. |
| Bounded liveness probe | `#ifdef __FreeBSD__`, plus `pam_launchd_probe.defs` | Before switching, the module asks the per-user launchd for its root bootstrap (`get_root_bootstrap`, a routine it answers itself without forwarding to PID 1) with a MIG `WaitTime` of 3000 ms. If it does not answer, the process stays on the root bootstrap and the module returns `PAM_IGNORE`. Without this, a per-user launchd that never checked in would leave every later bootstrap lookup in the session blocked forever. On a timeout, the MIG stub destroys the reply port, so a late reply cannot be mistaken for the reply to the next RPC. |
| `NOT_PRIVILEGED` branch reachable | unconditional | Apple tested `BOOTSTRAP_NOT_PRIVILEGED` after `!= BOOTSTRAP_SUCCESS`, which already catches it. The tests are swapped. The return value is the same (`PAM_IGNORE`); only the log message changes. |
| `#include <security/openpam.h>`, `PAM_MODULE_ENTRY` | `#ifdef __FreeBSD__` | OpenPAM declares `openpam_get_option`/`openpam_log` there. Every inherited FreeBSD module carries the module entry. |

`pam_launchd_probe.defs` is a client-side copy of one routine from
`src/launchd/src/job.defs` (subsystem `job 400`, routine index 36, request
id 436), with a `WaitTime` argument added. If `get_root_bootstrap` moves in
`job.defs`, move it in the probe too.

`pam_launchd.8` gains a NEXTBSD section (marked with a comment) and a FILES
section.

## Notes

- `_vproc_post_fork_ping()` is a no-op in NextBSD's liblaunch
  (`src/launchd/liblaunch/libvproc.c`), so that call always succeeds today.
- The default session type is `Background`, for which Apple skips
  `_vprocmgr_switch_to_session`. Only a non-default `launchd_session_type`
  makes the RPC.
