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
  `pam_launchd_probe.h` used by the probe below.

## NextBSD changes

All in `pam_launchd.c`, each marked with a `NextBSD:` comment.

| Change | Where | Why |
|---|---|---|
| Every failure returns `PAM_IGNORE` | `#ifdef __FreeBSD__` | Apple returned `PAM_SESSION_ERR` when the session switch or `_vproc_post_fork_ping` failed. On NextBSD a broken per-user session must degrade to today's behaviour (the root bootstrap), never block a login. On these paths the module also puts the process back on the root bootstrap before returning. |
| `MACH_PORT_NULL` per-user port is a failure | `#ifdef __FreeBSD__` | launchd-842's `job_mig_lookup_per_user_context` returns success with a null port if it could not create the per-user launchd. Apple would set a null bootstrap port. |
| Bounded liveness and forwarding probe | `#ifdef __FreeBSD__`, plus `pam_launchd_probe.defs` | Before switching, the module makes two calls through the per-user port, each with a MIG `WaitTime` of 3000 ms. First it asks the per-user launchd for its root bootstrap (`get_root_bootstrap`, a routine it answers itself). Then it looks up `com.apple.system.notification_center` (`look_up2`), notifyd's service, which only PID 1 has. The per-user launchd must forward that lookup to PID 1 (`vproc_mig_look_up2_forward`, `MIG_NO_REPLY`), and PID 1 replies to the module directly. The returned send right is released. If either call fails or times out, the process stays on the root bootstrap and the module returns `PAM_IGNORE`. Without this, a per-user launchd that never checked in, or one that cannot forward, would leave bootstrap lookups in the session blocked or failing. On a timeout, the MIG stub destroys the reply port, so a late reply cannot be mistaken for the reply to the next RPC. |
| `NOT_PRIVILEGED` branch reachable | unconditional | Apple tested `BOOTSTRAP_NOT_PRIVILEGED` after `!= BOOTSTRAP_SUCCESS`, which already catches it. The tests are swapped. The return value is the same (`PAM_IGNORE`); only the log message changes. |
| `#include <security/openpam.h>`, `PAM_MODULE_ENTRY` | `#ifdef __FreeBSD__` | OpenPAM declares `openpam_get_option`/`openpam_log` there. Every inherited FreeBSD module carries the module entry. |

`pam_launchd_probe.defs` holds client-side copies of two routines from
`src/launchd/src/job.defs` (subsystem `job 400`): `look_up2` (routine index
4, request id 404) and `get_root_bootstrap` (routine index 36, request id
436), each with a `WaitTime` argument added. If either moves or changes in
`job.defs`, change it in the probe too.

`pam_launchd.8` gains a NEXTBSD section (marked with a comment) and a FILES
section.

## Notes

- `_vproc_post_fork_ping()` is a no-op in NextBSD's liblaunch
  (`src/launchd/liblaunch/libvproc.c`), so that call always succeeds today.
- The default session type is `Background`, for which Apple skips
  `_vprocmgr_switch_to_session`. Only a non-default `launchd_session_type`
  makes the RPC. `launchd_session_type=StandardIO` is opt-in; see
  `pam_launchd.8`.
- `_vprocmgr_switch_to_session` (liblaunch, `libvproc.c`) passes launchd a
  requestor port whose death ends the new session. Darwin uses the task-name
  port. NextBSD has none yet (libmach's `task_name_for_pid` is a stub, the
  kernel's `sys_task_name_for_pid` is `ENOSYS` and unwired, and `itk_nself`
  is never created), and a null requestor made launchd refuse every switch.
  On NextBSD liblaunch now falls back to a receive right the process holds
  for its lifetime (`#ifdef __FreeBSD__` there).
