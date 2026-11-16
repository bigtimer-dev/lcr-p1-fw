# system reset (reloads option bytes), then halt and re-latch PA8. User holds TEST during this.
proc hx {v} { return [format 0x%08x $v] }
init
halt 1000
catch { write_memory 0xE000ED0C 32 0x05FA0004 }
sleep 100
catch { dap init }
catch { n32.cpu arp_examine }
halt 1000
set v [read_memory 0x40021018 32 1]
write_memory 0x40021018 32 [expr {$v | 4}]
write_memory 0x40010818 32 0x100
set m [read_memory 0x40010800 32 1]
write_memory 0x40010800 32 [expr {($m & ~(3 << 16)) | (1 << 16)}]
echo "LATCHED: GPIOA POD [hx [read_memory 0x40010814 32 1]] pc [reg pc]"
echo "OB [hx [read_memory 0x4002201C 32 1]] WRP [hx [read_memory 0x40022020 32 1]]"
echo "debugger flash read 0x08000000: [read_memory 0x08000000 32 4]"
echo "debugger flash read 0x0800F800: [read_memory 0x0800F800 32 2]"
shutdown
