/* cap.h -- per-task capability bits (design/app-runtime.md).
 *
 * The syscall (`int 0x80`) is the ONLY door between an app and the rest of the
 * system, so a capability bit checked at that boundary genuinely gates the
 * capability -- no ambient authority can route around it. Shared by the kernel
 * (which stores `task.caps` and enforces it in traps.c) and userspace (the
 * trusted launcher maps a manifest's `caps = ...` list onto these bits before
 * dropping the child to them at exec). Included via syscall.h so both sides agree.
 *
 * Tiers (design/app-runtime.md): NORMAL caps are granted at launch from the
 * manifest with no prompt; DANGEROUS caps (net, notify, camera, ...) start denied
 * and -- once the hardware + the runtime-prompt UI exist -- are granted per app at
 * first use. Today the only dangerous cap with a real syscall to gate is `notify`. */
#pragma once

#define CAP_WINDOW    (1u << 0)   /* win_create / present / window events     (normal)    */
#define CAP_FS_BUNDLE (1u << 1)   /* read its own .app bundle resources       (normal)    */
#define CAP_FS_HOME   (1u << 2)   /* read/write under the user home tree      (normal-)   */
#define CAP_FS_SYSTEM (1u << 3)   /* read-only the System tree (fonts, etc.)  (normal)    */
#define CAP_TIME      (1u << 4)   /* SYS_TIME / uptime                         (normal)    */
#define CAP_SPAWN     (1u << 5)   /* fork / exec (and the system file picker)  (normal)    */
#define CAP_NET       (1u << 6)   /* sockets / internet (when they exist)      (dangerous) */
#define CAP_NOTIFY    (1u << 7)   /* post desktop notifications to the WM      (dangerous) */

/* The caps a normal app gets with no manifest entry: everything low-risk, none of
 * the dangerous ones. (*fs:home stays here while tOS is single-user and unjailed --
 * Phase 3 narrows it to a per-app jail.) */
#define CAP_NORMAL  (CAP_WINDOW | CAP_FS_BUNDLE | CAP_FS_HOME | CAP_FS_SYSTEM | CAP_TIME | CAP_SPAWN)

/* The OS itself -- init and the boot chain (twm/shell/term) -- holds every cap. */
#define CAP_ALL     0xffffffffu
