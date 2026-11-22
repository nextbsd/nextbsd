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
#   - the WindowServer came up on QEMU's virtio-gpu (the harness gives both
#     arches one: q35's VGA plus virtio-gpu-pci, virt's virtio-gpu-pci) and
#     is serving: /var/log/WindowServer.log (the job's StandardErrorPath)
#     has "ready: <W>x<H>, service org.nextbsd.windowserver".
#   - loginwindow connected to it as a client ("client N (loginwindow, pid
#     P) connected" in the same log).
#   Measured on the first CI boots (nextbsd/nextbsd#684), amd64 and arm64,
#   disk image and live ISO: the virtio-gpu DRM device (card0, connector
#   Virtual-1) comes up at 1280x800, Mesa offers only llvmpipe so the
#   WindowServer draws with pixman, and loginwindow connects within about
#   15 seconds of boot, well before the serial login. The checks wait up to
#   90 seconds for each line anyway.
#
# What it reports, without gating: the jobs' launchctl list rows (PID and last
# exit status), the WindowServer's display, renderer, warning and error lines,
# and the end of loginwindow's log.
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
send "for j in WindowServer loginwindow; do r launchctl list org.nextbsd.\$j >/dev/null 2>&1 && s=loaded || s=missing; echo NB-DESK'-'JOB \$j \$s; done\r"
expect {
    timeout { puts "\nFAIL: DESKTOP — the launchd job check printed nothing within ${timeout}s"; exit 1 }
    -re $re_panic { puts "\nFAIL: kernel panic during the desktop check"; exit 1 }
    -re {NB-DESK-JOB ([A-Za-z]+) (loaded|missing)} {
        set nb_dj(org.nextbsd.$expect_out(1,string)) $expect_out(2,string)
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

# 3. The WindowServer's log: wait (up to 90 s each) until it is ready and
#    loginwindow has connected, then show the lines that matter, and the end
#    of loginwindow's own log. One short line per command: a 430-character
#    line lost everything past the same point on all four Desktop boots, so
#    each line stays well under 200 characters and waits for its own
#    sentinel.
set timeout 120
send "i=0; until r grep -q 'ready: ' /var/log/WindowServer.log || \[ \$i -ge 45 \]; do sleep 2; i=\$((i+1)); done; echo NB-WS'-'WAITED\r"
expect {
    timeout { puts "\nFAIL: DESKTOP — waiting for the WindowServer did not finish within ${timeout}s"; exit 1 }
    -re $re_panic { puts "\nFAIL: kernel panic during the desktop check"; exit 1 }
    "NB-WS-WAITED" {}
}
send "i=0; until r grep -q '(loginwindow, pid' /var/log/WindowServer.log || \[ \$i -ge 45 \]; do sleep 2; i=\$((i+1)); done; echo NB-LW'-'WAITED\r"
expect {
    timeout { puts "\nFAIL: DESKTOP — waiting for loginwindow did not finish within ${timeout}s"; exit 1 }
    -re $re_panic { puts "\nFAIL: kernel panic during the desktop check"; exit 1 }
    "NB-LW-WAITED" {}
}
set timeout 60
send "echo NB-WS'-'LOG-BEGIN; r grep -E 'error:|warning:|dri/card|renderer:|connected|ready:' /var/log/WindowServer.log | head -40; echo NB-WS'-'LOG-END\r"
set wslog ""
expect {
    timeout { puts "\nFAIL: DESKTOP — the WindowServer log dump did not finish within ${timeout}s"; exit 1 }
    -re $re_panic { puts "\nFAIL: kernel panic during the desktop check"; exit 1 }
    -re {NB-WS-LOG-BEGIN(.*)NB-WS-LOG-END} { set wslog $expect_out(1,string) }
}
send "r tail -n 15 /var/log/loginwindow.log; echo NB-LW'-'LOG-END\r"
expect {
    timeout { puts "\nWARN: DESKTOP — the loginwindow log dump did not finish" }
    "NB-LW-LOG-END" {}
}

if {[regexp {ld-elf[.]so[.]1: [^\r\n]*} $wslog ldmsg]} {
    puts "\nFAIL: DESKTOP-WINDOWSERVER — the WindowServer cannot run: $ldmsg"
    exit 1
}
if {![regexp {ready: ([0-9]+x[0-9]+), service org[.]nextbsd[.]windowserver} $wslog -> wsmode]} {
    if {[regexp {error: ([^\r\n]*)} $wslog -> wserr]} {
        puts "\nFAIL: DESKTOP-WINDOWSERVER — the WindowServer never became ready on QEMU's virtio-gpu: \"$wserr\""
    } else {
        puts "\nFAIL: DESKTOP-WINDOWSERVER — /var/log/WindowServer.log has no ready line within 90 s"
    }
    exit 1
}
puts "\nOK: DESKTOP-WINDOWSERVER — the WindowServer is ready at $wsmode"
if {![regexp {client [0-9]+ \(loginwindow, pid [0-9]+\) connected} $wslog]} {
    puts "\nFAIL: DESKTOP-LOGINWINDOW — loginwindow never connected to the WindowServer within 90 s"
    exit 1
}
puts "\nOK: DESKTOP-LOGINWINDOW — loginwindow connected to the WindowServer"
if {[regexp {renderer: ([^\r\n]*)} $wslog -> wsrend]} {
    puts "\nINFO: DESKTOP — renderer: $wsrend"
}
foreach w [regexp -all -inline {warning: [^\r\n]*} $wslog] {
    puts "\nINFO: DESKTOP — WindowServer $w"
}

set timeout $_saved_desktop_timeout
