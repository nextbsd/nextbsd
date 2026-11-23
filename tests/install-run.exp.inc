# install-run.exp.inc — on the live ISO, install onto the blank disk
# (nextbsd/nextbsd#686). tests/install-test.sh puts it after the harness's
# login and before its suite-tail (end-state hold) and teardown (poweroff).
# It uses what login.exp.inc leaves: a shell prompt, the patterns from
# patterns.tcl, and the r() wrapper (root, or sudo for admin).
#
# Steps:
#   1. The engine is on the medium: /usr/libexec/nextbsd-installer/
#      do-install.sh and probe-disks.sh (Installer.app's helper,
#      SettingsHelper's sh_install.c, runs them from there).
#   2. probe-disks.sh says SYSTEM image and offers vtbd0 (the blank disk) as
#      an install target: listed, and not marked as the boot medium.
#   3. do-install.sh -d vtbd0, as the helper runs it: PATH=/sbin:/usr/sbin:
#      /bin:/usr/bin, stdin on /dev/null. Gates on exit status 0, the four
#      PHASE lines (prepare, copy, setup, finish), PROGRESS reaching 100 and
#      the closing DONE. The wait has no fixed length: every PROGRESS, STATUS
#      or PHASE line restarts a 20-minute clock, so a slow (TCG) copy that is
#      still moving is never cut off, and a stalled one is.
#   4. What the engine left on vtbd0 (gpart), and whether a ZFS pool is still
#      imported on the live system: reported, not gated (the installed-disk
#      boot is what proves the result).
#
# Every command's output is read between two sentinels the command echoes;
# the typed line spells each sentinel with quotes in the middle, so the
# terminal's echo of the command can never match its output.

puts "\n==> install: the installer engine onto the blank disk (vtbd0)"
set _saved_install_timeout $timeout
match_max 1000000

# nb_run <tag> <command> [secs] — run one short shell command, return what it
# printed between NB-<tag>-B and NB-<tag>-E, with tabs shown as '|' (the
# tty may expand a tab).
proc nb_run {tag cmd {secs 120}} {
    global timeout re_panic expect_out
    set saved $timeout
    set timeout $secs
    send "echo NB-$tag'-'B; $cmd; echo NB-$tag'-'E\r"
    set out ""
    expect {
        timeout { puts "\nFAIL: INSTALL — '$cmd' did not finish within ${secs}s"; exit 2 }
        -re $re_panic { puts "\nFAIL: INSTALL — kernel panic during '$cmd'"; exit 1 }
        -re "NB-$tag-B(.*)NB-$tag-E" { set out $expect_out(1,string) }
    }
    set timeout $saved
    return [string map [list "\r" ""] $out]
}

set timeout 60
send "\r"
send "echo NB-INS'-'SYNC\r"
expect {
    timeout { puts "\nFAIL: INSTALL — no shell answered before the install"; exit 2 }
    -re $re_panic { puts "\nFAIL: INSTALL — kernel panic before the install"; exit 1 }
    "NB-INS-SYNC" {}
}

# 1. The engine.
set eng /usr/libexec/nextbsd-installer
set o [nb_run ENG "for f in do-install.sh probe-disks.sh; do test -x $eng/\$f && echo have \$f || echo lack \$f; done"]
foreach f {do-install.sh probe-disks.sh} {
    if {![regexp "have $f" $o]} {
        puts "\nFAIL: INSTALL-ENGINE — $eng/$f is missing from the live medium"
        exit 1
    }
}
puts "\nOK: INSTALL-ENGINE — $eng/do-install.sh and probe-disks.sh are on the medium"

# Who we are, for the record (the installed system is checked for the same
# account's home).
nb_run WHO {id; echo HOME=$HOME}

# 2. The probe.
set o [nb_run PROBE "r $eng/probe-disks.sh 2>&1 | tr '\\t' '|'" 120]
if {![regexp {SYSTEM\|image} $o]} {
    puts "\nFAIL: INSTALL-PROBE — probe-disks.sh does not say SYSTEM image on the live ISO"
    exit 1
}
if {![regexp {DISK\|vtbd0\|[^\n]*\|([01])\|([0-9]+)} $o -> media bytes]} {
    puts "\nFAIL: INSTALL-PROBE — probe-disks.sh does not offer vtbd0 (the blank disk)"
    exit 1
}
if {$media ne "0"} {
    puts "\nFAIL: INSTALL-PROBE — probe-disks.sh marks vtbd0 as the boot medium"
    exit 1
}
puts "\nOK: INSTALL-PROBE — probe-disks.sh offers vtbd0 ($bytes bytes), not as the medium"

# 3. The install.
puts "\n==> install: do-install.sh -d vtbd0"
set t_start [clock seconds]
set timeout 1200
set phases {}
set pct -1
set done 0
set inst_rc ""
send "r env -i PATH=/sbin:/usr/sbin:/bin:/usr/bin $eng/do-install.sh -d vtbd0 </dev/null 2>&1; echo NB-INST'-'RC=\$?\r"
# One arm for every engine line, so the earliest line in the buffer is the
# one matched: with an arm per kind, expect takes the first ARM that matches
# anywhere, and a later PROGRESS would swallow the PHASE line before it.
expect {
    -re $re_panic { puts "\nFAIL: INSTALL-RUN — kernel panic during the install"; exit 1 }
    -re {(PROGRESS|PHASE|STATUS|DONE|NB-INST-RC=)[ \t]*([^\r\n]*)\r?\n} {
        set k $expect_out(1,string)
        set v [string trim $expect_out(2,string)]
        switch -- $k {
            PROGRESS {
                if {[string is integer -strict $v] && $v > $pct} { set pct $v }
                exp_continue
            }
            PHASE { lappend phases $v; exp_continue }
            STATUS { exp_continue }
            NB-INST-RC= { set inst_rc $v }
            DONE { if {$v eq ""} { set done 1 }; exp_continue }
        }
    }
    timeout {
        puts "\nFAIL: INSTALL-RUN — do-install.sh printed nothing for ${timeout}s (last PROGRESS $pct, phases: $phases)"
        exit 2
    }
}
set t_inst [expr {[clock seconds] - $t_start}]
puts "\nINFO: INSTALL-RUN — do-install.sh took ${t_inst}s; exit $inst_rc; PROGRESS reached $pct; phases: $phases"
if {$inst_rc ne "0"} {
    puts "\nFAIL: INSTALL-RUN — do-install.sh -d vtbd0 exited $inst_rc"
    exit 1
}
foreach ph {prepare copy setup finish} {
    if {[lsearch -exact $phases $ph] < 0} {
        puts "\nFAIL: INSTALL-RUN — no PHASE $ph line"
        exit 1
    }
}
if {$pct != 100} {
    puts "\nFAIL: INSTALL-RUN — PROGRESS stopped at $pct"
    exit 1
}
if {!$done} {
    puts "\nFAIL: INSTALL-RUN — no DONE line"
    exit 1
}
puts "\nOK: INSTALL-RUN — do-install.sh -d vtbd0 finished (exit 0, PROGRESS 100, DONE) in ${t_inst}s"

# 4. What it left (informational).
set timeout 60
set o [nb_run PART "r gpart show -p vtbd0 2>&1"]
regsub -all {\n+} [string trim $o] " / " o1
puts "\nINFO: INSTALL — vtbd0 after the install: $o1"
set o [nb_run POOL "r zpool list -H -o name 2>/dev/null | sed 's/^/pool=/'"]
if {[regexp {pool=([A-Za-z0-9_.:-]+)} $o -> pool]} {
    puts "\nWARN: INSTALL — the pool $pool is still imported on the live system after the install (the engine should export it)"
} else {
    puts "\nINFO: INSTALL — no ZFS pool imported on the live system after the install"
}
nb_run MNT {mount -p | grep vtbd0 || echo none}

set timeout $_saved_install_timeout
