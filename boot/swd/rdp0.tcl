# remove read protection L1 -> L0 (N32G031 UM 2.2.1.9): unlock, option erase, RDP1=0xA5 -> hardware mass erase.
# CPU writes via FNIRSI loader gadget str r2,[r0,#0] @0x08000258. Never touches RDP2 (0x1FFFF60C).
proc cpu_str {a v} { reg r0 $a; reg r2 $v; step 0x08000258 }
proc rd {a} { return [read_memory $a 32 1] }
proc hx {v} { return [format 0x%08x $v] }
proc wait_done {what} {
    for {set i 0} {$i < 500} {incr i} { set s [rd 0x4002200C]; if {!($s & 1)} { echo "$what: STS [hx $s]"; return $s } }
    error "$what: busy timeout"
}
init
halt 1000
echo "OB [hx [rd 0x4002201C]] CTRL [hx [rd 0x40022010]]"
if {[rd 0x4002201C] & 0x80000000} { error "RDPRT2 set?!" }
cpu_str 0x40022004 0x45670123
cpu_str 0x40022004 0xCDEF89AB
set c [rd 0x40022010]; echo "after KEY: CTRL [hx $c]"
if {$c & 0x80} { error "flash still locked, abort" }
cpu_str 0x40022008 0x45670123
cpu_str 0x40022008 0xCDEF89AB
set c [rd 0x40022010]; echo "after OPTKEY: CTRL [hx $c]"
if {!($c & 0x200)} { error "OPTWE not set, abort" }
cpu_str 0x4002200C 0x34
cpu_str 0x40022010 0x220
cpu_str 0x40022010 0x260
set s [wait_done "option erase"]
cpu_str 0x40022010 0x200
set ob [read_memory 0x1FFFF600 32 4]; echo "option bytes after erase: $ob"
if {$s & 0x14} { error "erase error, abort" }
foreach v $ob { if {$v != 0xffffffff} { error "option page not erased, abort" } }
echo "writing RDP1 = 0xA5 (USER 0xFF) -> mass erase"
cpu_str 0x4002200C 0x34
cpu_str 0x40022010 0x210
cpu_str 0x1FFFF600 0x00FF00A5
wait_done "RDP1 program + mass erase"
echo "option bytes now: [read_memory 0x1FFFF600 32 4]"
echo "OB reg (old until reset) [hx [rd 0x4002201C]] CTRL [hx [rd 0x40022010]]"
shutdown
