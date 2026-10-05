# RevolutionOS

A FreeBSD based operating system derived from a forked and edited version of nextbsd 

here are some commands you can try out yourself. neat, huh?
```sh
# launchctl — the Darwin service-control tool, manages launchd jobs.
launchctl list                       # show every job launchd knows about
launchctl list com.apple.syslogd     # plist for one specific job

# ipconfig — Darwin's IP-config CLI (NOT the same as ifconfig)
ipconfig getifaddr em0
ipconfig ifcount

# ioreg — Darwin's hardware-registry browser, over the in-kernel IORegistry
ioreg -l                             # the full registry tree
ioreg -c PCIDevice                   # filter by class
ioreg -n hostb0                      # find by name

# syslog — Darwin's log-query tool, served by syslogd's ASL store
syslog -F bsd                        # tail the system log in BSD format
syslog -k Sender ipconfigd           # filter by sender

# version identity — NextBSD's own
uname -a                             # ostype reads NextBSD
nextbsd-version                      # userland build version (a timestamp)
nextbsd-version -k                   # the installed kernel's version

# Standard tools are all still here
top
ps aux
pkg info
```

## Versioning

NextBSD stamps every build with a single UTC timestamp
(`YYYYMMDD-HHMMSS`), computed once and shared by the image/ISO names,
`/etc/os-release`, and `nextbsd-version` — so they always agree.

```sh
nextbsd-version          # userland build version, e.g. 20260613-224731
nextbsd-version -k       # installed kernel version (its own build time)
nextbsd-version -r       # running kernel version (= uname -r)
cat /etc/os-release      # NAME=NextBSD, VERSION_ID=<same timestamp>, ...
```

Userland and kernel build in separate repos at different times, so
`nextbsd-version` (userland) and `-k`/`-r` (kernel) legitimately differ;
that gap is exactly what the command exists to show.


## License

[BSD-2-Clause](LICENSE), with per-component Apache 2.0 / LGPL / MIT /
OSF / CMU headers preserved on imported files — see
[NOTICE](NOTICE).
