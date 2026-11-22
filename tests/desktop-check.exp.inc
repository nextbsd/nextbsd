# desktop-check.exp.inc — the Desktop Preview's check on the booted system
# (nextbsd/nextbsd#678). img-boot-test.sh / iso-boot-test.sh splice it into
# the shared nextbsd-ci harness when NB_DESKTOP=1: after the login (login-only
# boots have no suite block), before suite-tail's end-state hold and the
# poweroff. It uses what login.exp.inc leaves behind: a shell prompt, the
# patterns from patterns.tcl, and the r() wrapper (root, or sudo for admin).
#
# What it gates on:
#   - org.nextbsd.WindowServer and org.nextbsd.loginwindow are loaded in
#     launchd. NextBSD-desktop-apps' post-install script enabled both in the
#     overrides database; build.sh checked the database, this checks that
#     launchd read it.
#   - launchd started the WindowServer: /var/log/WindowServer.log (the job's
#     StandardErrorPath) has its startup line, and no run-time linker error.
#
# What it reports, without gating: the jobs' launchctl list rows (PID and last
# exit status) and the end of the WindowServer's and loginwindow's logs,
# which say whether the WindowServer found a display on QEMU's virtio-gpu.
# CI's console is serial and nobody looks at the screen, so whether a display
# came up there is a fact about the emulator, not a verdict on the image.
#
# The echo'd sentinels are split with quotes ('-' or "" in the middle) so the
# terminal's echo of a typed line can never match the output it waits for.

puts "\n==> desktop check: WindowServer and loginwindow"
set _saved_desktop_timeout $timeout
match_max 65536

set timeout 60
send "\r"
send "echo NB-DESK'-'SYNC\r"
expect {
    timeout { puts "\nFAIL: DESKTOP — no shell answered before the desktop check"; exit 2 }
    -re $re_panic { puts "\nFAIL: kernel panic before the desktop check"; exit 1 }
    "NB-DESK-SYNC" {}
}

# 1. Both jobs loaded (launchctl list <label> exits non-zero for an unknown
#    job: the getty job's own test for loginwindow).
array set nb_dj {}
send "for j in org.nextbsd.WindowServer org.nextbsd.loginwindow; do if r launchctl list \$j >/dev/null 2>&1; then s=loaded; else s=missing; fi; echo \"NB-DESK\"\"TOP-JOB \$j \$s\"; done\r"
expect {
    timeout { puts "\nFAIL: DESKTOP — the launchd job check printed nothing within ${timeout}s"; exit 1 }
    -re $re_panic { puts "\nFAIL: kernel panic during the desktop check"; exit 1 }
    -re {NB-DESKTOP-JOB (org\.nextbsd\.[A-Za-z]+) (loaded|missing)} {
        set nb_dj($expect_out(1,string)) $expect_out(2,string)
        if {[array size nb_dj] < 2} { exp_continue }
    }
}
foreach j {org.nextbsd.WindowServer org.nextbsd.loginwindow} {
    if {![info exists nb_dj($j)] || $nb_dj($j) ne "loaded"} {
        puts "\nFAIL: DESKTOP-JOBS — $j is not loaded in launchd (NextBSD-desktop-apps enables it in the overrides database)"
        exit 1
    }
}
puts "\nOK: DESKTOP-JOBS — org.nextbsd.WindowServer and org.nextbsd.loginwindow are loaded"

# 2. Their launchctl list rows: PID ('-' when not running) and last exit
#    status. Informational.
send "r launchctl list | grep -E 'org.nextbsd.(WindowServer|loginwindow)'; echo NB-DESK'-'LIST-END\r"
expect {
    timeout { puts "\nWARN: DESKTOP — launchctl list printed nothing within ${timeout}s" }
    "NB-DESK-LIST-END" {}
}

# 3. The WindowServer's log. launchd starts the job as soon as it loads it,
#    long before the login above, so the log is normally there already; give
#    it up to a minute anyway. Then show its end, and loginwindow's.
set timeout 120
send "i=0; until r test -s /var/log/WindowServer.log || \[ \$i -ge 30 \]; do sleep 2; i=\$((i+1)); done; echo NB-WS'-'LOG-BEGIN; r tail -n 40 /var/log/WindowServer.log; echo NB-WS'-'LOG-END; r tail -n 20 /var/log/loginwindow.log; echo NB-LW'-'LOG-END\r"
set wslog ""
expect {
    timeout { puts "\nFAIL: DESKTOP — the WindowServer log dump did not finish within ${timeout}s"; exit 1 }
    -re $re_panic { puts "\nFAIL: kernel panic during the desktop check"; exit 1 }
    -re {NB-WS-LOG-BEGIN(.*)NB-WS-LOG-END} { set wslog $expect_out(1,string) }
}
expect {
    timeout { puts "\nWARN: DESKTOP — the loginwindow log dump did not finish" }
    "NB-LW-LOG-END" {}
}

if {![regexp {WindowServer\[[0-9]+\]} $wslog]} {
    puts "\nFAIL: DESKTOP-WINDOWSERVER — /var/log/WindowServer.log is empty or has no WindowServer line: launchd never started it"
    exit 1
}
if {[regexp {ld-elf[.]so[.]1: [^\r\n]*} $wslog ldmsg]} {
    puts "\nFAIL: DESKTOP-WINDOWSERVER — the WindowServer cannot run: $ldmsg"
    exit 1
}
puts "\nOK: DESKTOP-WINDOWSERVER — launchd started the WindowServer"
if {[regexp {(/dev/dri/card[0-9]+): [0-9]+ displays? connected} $wslog -> card]} {
    puts "\nINFO: DESKTOP-DISPLAY — the WindowServer is driving a display on $card"
} elseif {[regexp {error: ([^\r\n]*)} $wslog -> wserr]} {
    puts "\nINFO: DESKTOP-DISPLAY — no display: the WindowServer says \"$wserr\" (not gated: see tests/desktop-check.exp.inc)"
} else {
    puts "\nINFO: DESKTOP-DISPLAY — the WindowServer log names no display yet (not gated)"
}

set timeout $_saved_desktop_timeout
