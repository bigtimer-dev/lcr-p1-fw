init
if {[catch {halt 1000} e]} { echo "HALT ERR: $e" }
echo "state: [n32.cpu curstate]"
set v [read_memory 0x40021018 32 1]
write_memory 0x40021018 32 [expr {$v | 4}]
write_memory 0x40010818 32 0x100
set m [read_memory 0x40010800 32 1]
write_memory 0x40010800 32 [expr {($m & ~(3 << 16)) | (1 << 16)}]
echo "GPIOA MODE [format %08x [read_memory 0x40010800 32 1]] POD [format %08x [read_memory 0x40010814 32 1]] (PA8 latch = bit 8)"
echo "[reg pc]"
echo "[reg xpsr]"
echo "IWDG PR/RLR [read_memory 0x40003004 32 2]"
shutdown
