# read-only: option bytes + 0x0800F800-0x0800FFFF through the CPU (FNIRSI loader gadget ldr r1,[r1,#0] @0x08000392)
proc rdw {a} { reg r1 $a; step 0x08000392; regexp {0x[0-9a-fA-F]+} [reg r1] v; return $v }
init
halt 1000
echo "state: [n32.cpu curstate] pc [reg pc]"
echo "OB [format %08x [read_memory 0x4002201C 32 1]] WRP [format %08x [read_memory 0x40022020 32 1]] CTRL [format %08x [read_memory 0x40022010 32 1]] STS [format %08x [read_memory 0x4002200C 32 1]]"
echo "check 0x08000000 = [rdw 0x08000000] (want 0x20000838)"
foreach a {0x1FFFF600 0x1FFFF604 0x1FFFF608 0x1FFFF60C} { echo "O $a [rdw $a]" }
catch { echo "O-dbg [read_memory 0x1FFFF600 32 4]" } e; echo "dbg read: $e"
for {set a 0x0800F800} {$a < 0x08010000} {incr a 4} { echo "W [format 0x%08x $a] [rdw $a]" }
echo "check again 0x08000004 = [rdw 0x08000004] (want 0x080000d9)"
shutdown
