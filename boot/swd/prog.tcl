# L0: program flash from the debugger (N32G031 UM 2.2.1.4), image already mass-erased
proc hx {v} { return [format 0x%08x $v] }
source prog_data.tcl
init
halt 1000
if {[read_memory 0x4002201C 32 1] & 2} { error "still read-protected" }
write_memory 0x40022004 32 0x45670123
write_memory 0x40022004 32 0xCDEF89AB
echo "CTRL [hx [read_memory 0x40022010 32 1]]"
write_memory 0x4002200C 32 0x34
write_memory 0x40022010 32 1
set n 0; set err 0
foreach {a v} $words {
    write_memory $a 32 $v
    set s [read_memory 0x4002200C 32 1]
    while {$s & 1} { set s [read_memory 0x4002200C 32 1] }
    if {$s & 0x14} { incr err; if {$err < 5} { echo "ERR [hx $a] STS [hx $s]" }; write_memory 0x4002200C 32 0x34 }
    incr n
    if {$n % 2000 == 0} { echo "$n words" }
}
write_memory 0x40022010 32 0x80
echo "programmed $n words, $err errors"
dump_image readback.bin 0x08000000 0x10000
shutdown
