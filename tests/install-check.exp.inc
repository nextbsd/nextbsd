# install-check.exp.inc — on the installed disk, booted alone, check what the
# installer made (nextbsd/nextbsd#686). tests/install-test.sh puts it after
# the harness's login (which is itself the first check: the installed system
# booted to a login and the account logged in), before desktop-check.exp.inc
# on a Desktop image, and before the harness's end-state hold and poweroff.
#
# The file system is read off the booted system, not assumed, so the one test
# covers both engines:
#
#   ZFS  (the root is ZFS, or a NextBSD pool is imported: the engine of
#        nextbsd/nextbsd-userland#658). Strict, every item gates:
#          - pool NextBSD, ONLINE, bootfs NextBSD/ROOT/<BE>, where <BE> is the
#            installed system's version and build (27.0.0-192, from sw_vers;
#            nextbsd/nextbsd-userland#674), or default from an installer
#            before that
#          - / is the bootfs, mounted read-write (and writable)
#          - the six datasets exist, are mounted, at their mountpoints:
#              NextBSD/ROOT/<BE>        /
#              NextBSD/Local            /Local
#              NextBSD/root             /root
#              NextBSD/private/tmp      /private/tmp
#              NextBSD/private/var/log  /private/var/log
#              NextBSD/private/var/tmp  /private/var/tmp
#          - kern.hostid is not zero (the boot pass, nextbsd-userland#200)
#          - bectl list shows <BE> active now and on reboot (NR)
#          - no vfs.root.mountfrom line in /boot/loader.conf (bootfs decides)
#          - vtbd0 is GPT with an efi and a freebsd-zfs partition
#   UFS  (today's engine). / is /dev/ufs/NEXTBSD (the installed system's own
#        label, never the medium's ROOTFS), ufs, read-write and writable;
#        loader.conf names it; vtbd0 is GPT with efi + freebsd-ufs.
#
# Both, gating:
#   - the logged-in account is not root, and its home is a directory under
#     /Local/Users that it owns (on ZFS, inside the NextBSD/Local dataset);
#   - directory services: dsstatus runs and says standalone, and dspromote's
#     precondition holds -- /Local/Library/DirectoryServices and /Local/Users
#     are on /Local's file system (ds.c write_exports refuses otherwise).
#     Nothing is promoted.
# Reported only: kern.hostid on UFS, the mount table, df.
#
# Every command's output is read between two sentinels the command echoes;
# the typed line spells each sentinel with quotes in the middle, so the
# terminal's echo of the command can never match its output. Lines stay
# short (a 430-character line was cut on the serial console, desktop-check).

puts "\n==> installed system: what the installer made"
set _saved_check_timeout $timeout
match_max 1000000

proc nb_run {tag cmd {secs 120}} {
    global timeout re_panic expect_out
    set saved $timeout
    set timeout $secs
    send "echo NB-$tag'-'B; $cmd; echo NB-$tag'-'E\r"
    set out ""
    expect {
        timeout { puts "\nFAIL: INSTALLED — '$cmd' did not finish within ${secs}s"; exit 2 }
        -re $re_panic { puts "\nFAIL: INSTALLED — kernel panic during '$cmd'"; exit 1 }
        -re "NB-$tag-B(.*)NB-$tag-E" { set out $expect_out(1,string) }
    }
    set timeout $saved
    return [string map [list "\r" ""] $out]
}

# nb_val <text> <key> — the value of the first "NBV <key> <value>" line.
proc nb_val {text key} {
    if {[regexp -line "^NBV $key (.*)\$" $text -> v]} { return [string trim $v] }
    return ""
}

set nb_fail 0
proc nb_bad {msg} {
    global nb_fail
    puts "\nFAIL: $msg"
    incr nb_fail
}

set timeout 60
send "\r"
send "echo NB-CHK'-'SYNC\r"
expect {
    timeout { puts "\nFAIL: INSTALLED — no shell answered after the login"; exit 2 }
    -re $re_panic { puts "\nFAIL: INSTALLED — kernel panic after the login"; exit 1 }
    "NB-CHK-SYNC" {}
}

# --- what is the root, and is there a pool --------------------------------
set o [nb_run ROOT {mount -p | awk '$2=="/" {print "NBV rootdev", $1; print "NBV roottype", $3; print "NBV rootopts", $4}'}]
set rootdev  [nb_val $o rootdev]
set roottype [nb_val $o roottype]
set rootopts [nb_val $o rootopts]
set o [nb_run POOLS {r zpool list -H -o name,health 2>/dev/null | awk '{print "NBV pool", $1, $2}'}]
set pools [regexp -all -inline -line {^NBV pool .*$} $o]
puts "\nINFO: INSTALLED — root $rootdev ($roottype, $rootopts); pools: [llength $pools]"

if {$roottype eq "zfs" || [regexp {NBV pool NextBSD} $o]} {
    set fs zfs
} elseif {$roottype eq "ufs"} {
    set fs ufs
} else {
    puts "\nFAIL: INSTALLED-FS — the root is '$roottype' ($rootdev): neither the ZFS nor the UFS install"
    exit 1
}
puts "\nINFO: INSTALLED-FS — the installer made a [string toupper $fs] system"

# --- / is writable (touch updates the directory's times: no file is made) --
set o [nb_run RW {r touch / && echo NBV rw yes || echo NBV rw no}]
if {[nb_val $o rw] ne "yes"} { nb_bad "INSTALLED-ROOT-RW — / is not writable" }
if {![regexp {(^|,)rw(,|$)} $rootopts]} { nb_bad "INSTALLED-ROOT-RW — / is mounted '$rootopts', not rw" }

set o [nb_run LCONF {grep '^vfs.root.mountfrom' /boot/loader.conf | sed 's/^/NBV lc /'; echo NBV end}]
set lcroot [nb_val $o lc]
set o [nb_run PARTS {r gpart show -p vtbd0 2>&1 | awk 'NF >= 4 {print "NBV part", $3, $4}'}]
set parts [regexp -all -inline -line {^NBV part .*$} $o]
puts "\nINFO: INSTALLED — vtbd0: [join $parts { / }]"
set hostid [nb_val [nb_run HOSTID {sysctl -n kern.hostid | sed 's/^/NBV hostid /'}] hostid]

if {$fs eq "zfs"} {
    # --- the pool ----------------------------------------------------------
    set o [nb_run POOLS2 {r zpool list -H -o name,health 2>&1 | awk '{print "NBV pool", $1, $2}'}]
    if {![regexp -line {^NBV pool NextBSD (\S+)$} $o -> health]} {
        nb_bad "INSTALLED-ZFS-POOL — no pool named NextBSD (pools: [join $pools { / }])"
    } elseif {$health ne "ONLINE"} {
        nb_bad "INSTALLED-ZFS-POOL — NextBSD is $health, not ONLINE"
    } else {
        puts "\nOK: INSTALLED-ZFS-POOL — pool NextBSD is ONLINE"
    }
    set o [nb_run BOOTFS {r zpool get -H -o value bootfs NextBSD 2>&1 | sed 's/^/NBV bootfs /'; echo "NBV swvers $(sw_vers -productVersion)-$(sw_vers -buildVersion)"}]
    set bootfs [nb_val $o bootfs]
    # The boot environment is named after the system in it (27.0.0-192);
    # an installer from before nextbsd/nextbsd-userland#674 named it default.
    set bename [nb_val $o swvers]
    set be "NextBSD/ROOT/$bename"
    if {$bootfs eq "NextBSD/ROOT/default"} {
        set be $bootfs
        set bename default
    }
    if {$bootfs ne $be} {
        nb_bad "INSTALLED-ZFS-BOOTFS — bootfs is '$bootfs', not $be"
    } else {
        puts "\nOK: INSTALLED-ZFS-BOOTFS — bootfs $be"
    }
    if {$rootdev ne $be || $roottype ne "zfs"} {
        nb_bad "INSTALLED-ZFS-ROOT — / is $rootdev ($roottype), not $be (zfs)"
    } else {
        puts "\nOK: INSTALLED-ZFS-ROOT — / is $be"
    }

    # --- the six datasets ---------------------------------------------------
    set o [nb_run DS {r zfs list -H -o name,mounted,mountpoint -r NextBSD 2>&1 | awk '{print "NBV ds", $1, $2, $3}'}]
    set m [nb_run ZMNT {mount -p | awk '$3 == "zfs" {print "NBV zm", $1, $2, $4}'}]
    foreach {ds mp} [list \
        $be                     / \
        NextBSD/Local           /Local \
        NextBSD/root            /root \
        NextBSD/private/tmp     /private/tmp \
        NextBSD/private/var/log /private/var/log \
        NextBSD/private/var/tmp /private/var/tmp \
    ] {
        set mounted ""; set zmp ""
        regexp -line "^NBV ds $ds (\\S+) (\\S+)\$" $o -> mounted zmp
        if {$mounted eq "" && $zmp eq ""} {
            nb_bad "INSTALLED-ZFS-DATASETS — $ds does not exist"
            continue
        }
        if {$zmp ne $mp} {
            nb_bad "INSTALLED-ZFS-DATASETS — $ds has mountpoint '$zmp', not $mp"
            continue
        }
        if {$mounted ne "yes"} {
            nb_bad "INSTALLED-ZFS-DATASETS — $ds is not mounted (mounted=$mounted)"
            continue
        }
        if {![regexp -line "^NBV zm $ds $mp (\\S+)\$" $m -> opts]} {
            nb_bad "INSTALLED-ZFS-DATASETS — $ds is not in the mount table at $mp"
            continue
        }
        if {![regexp {(^|,)rw(,|$)} $opts]} {
            nb_bad "INSTALLED-ZFS-DATASETS — $ds is mounted '$opts', not rw"
            continue
        }
        puts "\nOK: INSTALLED-ZFS-DATASETS — $ds mounted rw at $mp"
    }

    # --- host ID, boot environments, loader.conf, partitions -----------------
    if {![string is integer -strict $hostid] || $hostid == 0} {
        nb_bad "INSTALLED-ZFS-HOSTID — kern.hostid is '$hostid' (the boot pass sets it)"
    } else {
        puts "\nOK: INSTALLED-ZFS-HOSTID — kern.hostid $hostid"
    }
    set o [nb_run BE {r bectl list -H 2>&1 | awk '{print "NBV be", $1, $2}'}]
    if {![regexp -line "^NBV be [string map {. \\.} $bename] (\\S+)\$" $o -> act]} {
        nb_bad "INSTALLED-ZFS-BE — bectl list has no boot environment named $bename"
    } elseif {[string first N $act] < 0 || [string first R $act] < 0} {
        nb_bad "INSTALLED-ZFS-BE — boot environment $bename is '$act', not active now and on reboot (NR)"
    } else {
        puts "\nOK: INSTALLED-ZFS-BE — bectl: $bename is active ($act)"
    }
    if {$lcroot ne ""} {
        nb_bad "INSTALLED-ZFS-LOADER — loader.conf has a root line ($lcroot); the pool's bootfs should decide"
    } else {
        puts "\nOK: INSTALLED-ZFS-LOADER — no vfs.root.mountfrom in loader.conf (bootfs decides)"
    }
    if {[lsearch -glob $parts {* efi}] < 0 || [lsearch -glob $parts {* freebsd-zfs}] < 0} {
        nb_bad "INSTALLED-ZFS-GPT — vtbd0 lacks an efi or a freebsd-zfs partition"
    } else {
        puts "\nOK: INSTALLED-ZFS-GPT — vtbd0 has efi + freebsd-zfs"
    }
} else {
    # --- UFS ---------------------------------------------------------------
    if {![regexp {^/dev/ufs/NEXTBSD(-[0-9a-f]+)?$} $rootdev]} {
        nb_bad "INSTALLED-UFS-ROOT — / is $rootdev, not the installed system's /dev/ufs/NEXTBSD"
    } else {
        puts "\nOK: INSTALLED-UFS-ROOT — / is $rootdev (ufs, $rootopts)"
    }
    if {![string match "*ufs:/dev/ufs/NEXTBSD*" $lcroot]} {
        nb_bad "INSTALLED-UFS-LOADER — loader.conf's root line is '$lcroot', not ufs:/dev/ufs/NEXTBSD"
    } else {
        puts "\nOK: INSTALLED-UFS-LOADER — loader.conf: $lcroot"
    }
    if {[lsearch -glob $parts {* efi}] < 0 || [lsearch -glob $parts {* freebsd-ufs}] < 0} {
        nb_bad "INSTALLED-UFS-GPT — vtbd0 lacks an efi or a freebsd-ufs partition"
    } else {
        puts "\nOK: INSTALLED-UFS-GPT — vtbd0 has efi + freebsd-ufs"
    }
    puts "\nINFO: INSTALLED — kern.hostid $hostid"
}

# --- the account and its home ------------------------------------------------
set o [nb_run HOME {echo NBV user $(id -un); echo NBV home $HOME; stat -f 'NBV owner %Su' $HOME 2>&1; test -d $HOME && echo NBV isdir yes}]
set user [nb_val $o user]
set home [nb_val $o home]
set df [nb_run HOMEDF {df $HOME | awk 'NR == 2 {print "NBV fs", $1, $NF}'}]
set homefs [nb_val $df fs]
if {$user eq "" || $user eq "root"} {
    nb_bad "INSTALLED-LOGIN — logged in as '$user', not a normal account (admin)"
} elseif {![string match "/Local/Users/?*" $home]} {
    nb_bad "INSTALLED-HOME — $user's home is '$home', not under /Local/Users"
} elseif {[nb_val $o isdir] ne "yes" || [nb_val $o owner] ne $user} {
    nb_bad "INSTALLED-HOME — $home is missing or not owned by $user ([nb_val $o owner])"
} elseif {$fs eq "zfs" && ![string match "NextBSD/Local *" $homefs] &&
          ![string match "NextBSD/Local/Users/$user *" $homefs]} {
    # A home is a dataset of its own since nextbsd/nextbsd-userland#673.
    nb_bad "INSTALLED-HOME — $home is on '$homefs', not its own NextBSD/Local/Users dataset or NextBSD/Local"
} else {
    puts "\nOK: INSTALLED-HOME — logged in as $user; home $home exists, owned by $user, on $homefs"
}

# --- directory services: dsstatus, and dspromote's precondition ----------------
set o [nb_run DSST {PATH=$PATH:/usr/sbin:/sbin dsstatus 2>&1; echo NBV dsrc $?}]
if {[nb_val $o dsrc] ne "0" || ![regexp {standalone} $o]} {
    nb_bad "INSTALLED-DS — dsstatus did not say standalone (exit [nb_val $o dsrc])"
} else {
    puts "\nOK: INSTALLED-DS — dsstatus: standalone"
}
set o [nb_run DSDEV {for p in /Local /Local/Library /Local/Library/DirectoryServices /Local/Users; do stat -f 'NBV dev %d %N' $p 2>&1; done}]
set devs [regexp -all -inline -line {^NBV dev \S+ \S+$} $o]
set ldev ""
regexp -line {^NBV dev (\S+) /Local$} $o -> ldev
set bad {}
foreach p {/Local/Library /Local/Library/DirectoryServices /Local/Users} {
    if {![regexp -line "^NBV dev (\\S+) $p\$" $o -> d] || $d ne $ldev} { lappend bad $p }
}
if {$ldev eq "" || [llength $bad] > 0} {
    nb_bad "INSTALLED-DS-PROMOTE — not on /Local's file system: $bad (dspromote would refuse to export /Local)"
} else {
    puts "\nOK: INSTALLED-DS-PROMOTE — /Local/Library, its DirectoryServices and /Local/Users are on /Local's file system"
}

# --- for the record ---------------------------------------------------------
nb_run MOUNTS {mount -p; df -h}

if {$nb_fail > 0} {
    puts "\nFAIL: INSTALLED — $nb_fail check(s) failed on the installed [string toupper $fs] system"
    exit 1
}
puts "\nOK: INSTALLED — the installed [string toupper $fs] system passed every check"
set timeout $_saved_check_timeout
