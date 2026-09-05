/*
 * gateway.r - Gateway's resources.
 *
 * Written as raw data blocks rather than through "Processes.r" and "Types.r",
 * because Rez runs against whichever RIncludes are linked into the toolchain at
 * build time and this file has to survive both the Universal and the
 * Multiversal arrangement.
 */

/*
 * SIZE (-1): 8 MB preferred, 4 MB minimum (CLAUDE.md rule 5). Several
 * listeners, a 32 KB bounce buffer per splice and BearSSL's X.509 parsing all
 * want room, and Certainly's TLS 1.3 write path puts a 16 KB record buffer on
 * the stack.
 *
 * Flags word 0x5880:
 *   0x4000  acceptSuspendResumeEvents
 *   0x1000  canBackground            - Gateway proxies while it is behind
 *   0x0800  doesActivateOnFGSwitch
 *   0x0080  is32BitCompatible
 */
data 'SIZE' (-1, "Gateway", purgeable) {
    $"5880"
    $"0080 0000"                        /* preferred: 8388608 bytes */
    $"0040 0000"                        /* minimum:   4194304 bytes */
};

/*
 * vers (1): 0.1 development. Byte layout is major(BCD), minor(BCD), stage,
 * prerelease, region, then two Pascal strings.
 */
data 'vers' (1, purgeable) {
    $"00 10 20 00 0000"
    $"03" "0.1"
    $"19" "0.1, Gateway for Mac OS 9"
};

data 'vers' (2, purgeable) {
    $"00 10 20 00 0000"
    $"03" "0.1"
    $"07" "Gateway"
};
