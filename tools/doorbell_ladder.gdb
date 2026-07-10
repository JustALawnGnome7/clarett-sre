# doorbell_ladder.gdb — per-command response ladder of a vendor bring-up (wall §7).
#
# Attach to the trace-enabled QEMU running the FC guest and, at EVERY FCP doorbell
# submit (BAR0 0x408 <- 1), append the current 4 KB response buffer to /tmp/ladder.bin.
# Record N therefore holds the device's response to command N-1 — a complete
# (seq -> error code, payload) record of the bring-up at native speed, including the
# 30 ms command storm that pmemsave sampling cannot resolve.
#
# The response-buffer GPA is learned live from the guest's own 0x410/0x414 writes and
# translated to a QEMU host VA via the pc.ram block (q35, 2 GiB low-RAM split: GPA
# below 2G maps 1:1; GPA >= 4G maps at host_base + GPA - 4G + 2G). gdb then reads the
# buffer straight out of the QEMU process — no inferior calls, no guest perturbation
# beyond the per-hit breakpoint stop.
#
# Usage (run BEFORE the guest reaches the Focusrite driver; see clarett-respbuf-plan.md):
#   sudo rm -f /tmp/ladder.bin && sudo touch /tmp/ladder.bin
#   sudo gdb -p "$(pgrep -f '^/usr/local/bin/qemu-system-x86_64' | head -1)" -x tools/doorbell_ladder.gdb
#   ... wait for "LADDER: armed" then the hit counter; Ctrl-C + detach + quit when done.
#   Decode: python3 tools/resp_dump.py --ladder /tmp/ladder.bin
#
# PID SELECTION: select the custom (trace-enabled, debug-info) QEMU by its /usr/local/bin
#   path — NOT `pgrep -f 'qemu.*Windows10'`, which also matches the stripped stock-QEMU
#   Windows10-WinDbg debugger VM and makes gdb fail at the ram_list read.
# NO MANUAL SIGNAL HANDLING NEEDED: the `handle SIGUSR1/2` lines below pass QEMU's
#   vCPU-kick signals through, so gdb continues immediately with no long stop. (A long
#   stop starves libvirt's monitor keepalive → it drops the domain and virt-manager
#   crashes; the per-doorbell stops here are sub-ms and safe.) Output is appended per hit,
#   so once the hit counter reaches ~85 the capture is already saved to disk regardless.
#
# Troubleshooting: if the first records decode as garbage, the low-RAM split differs —
# check `sudo virsh qemu-monitor-command <dom> --hmp 'info mtree -f'` for where pc.ram
# is mapped above 4G and adjust $below4g. If gdb reports addr/data <optimized out>,
# move the breakpoint to the trace_vfio_region_write() call line in hw/vfio/common.c.

set pagination off
set confirm off

# QEMU kicks vCPU threads with SIGUSR1 (and may use SIGUSR2); gdb must pass these
# through silently or it halts the whole VM on the first kick.
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass

# find the pc.ram RAMBlock host base
set $rb = ram_list.blocks.lh_first
while $rb != 0 && !$_streq($rb->idstr, "pc.ram")
  set $rb = $rb->next.le_next
end
if $rb == 0
  printf "LADDER: pc.ram block not found — aborting\n"
  quit
end
set $H = $rb->host
set $below4g = 0x80000000
printf "LADDER: pc.ram host base %p\n", $H

set $lo = (unsigned long)-1
set $hi = (unsigned long)-1
set $resp = (unsigned char *)0
set $hits = 0

break vfio_region_write
commands
  silent
  if addr == 0x410
    set $lo = data
  end
  if addr == 0x414
    set $hi = data
  end
  if $resp == 0 && $lo != (unsigned long)-1 && $hi != (unsigned long)-1
    set $gpa = ($hi << 32) | $lo
    if $gpa >= 0x100000000
      set $off = $gpa - 0x100000000 + $below4g
    else
      set $off = $gpa
    end
    set $resp = $H + $off
    printf "LADDER: armed — resp GPA=0x%lx HVA=%p\n", $gpa, $resp
  end
  if $resp != 0 && addr == 0x408 && data == 1
    append binary memory /tmp/ladder.bin $resp $resp+4096
    set $hits = $hits + 1
    printf "LADDER: hit %d\n", $hits
  end
  continue
end

printf "LADDER: breakpoint set — continuing; Ctrl-C to stop after the bring-up\n"
continue
