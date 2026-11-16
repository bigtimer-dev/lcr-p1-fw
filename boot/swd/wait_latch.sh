#!/bin/sh
# wait (max 10 min) for chip power (SWDIO pulled up), then halt the core and switch on the PA8 power latch
cd "$(dirname "$0")"
end=$(( $(date +%s) + 600 ))
powered() { pinctrl set 24 ip pd; l=$(pinctrl lev 24); pinctrl set 24 ip pn; [ "$l" = 1 ]; }
while [ $(date +%s) -lt $end ]; do
    if powered; then
        echo "chip powered at $(date +%T.%N)"
        sleep 0.3
        python3 pads.py 0x08
        for i in 1 2 3 4 5 6 7 8; do
            echo "--- try $i $(date +%T.%N)"
            python3 raw.py 100e-6
            python3 pads.py 0x08 > /dev/null
            if openocd -f n32slow.cfg -f latch.tcl > try$i.log 2>&1 && grep -q "GPIOA MODE" try$i.log; then
                grep -v -E "^(Open On|Licensed|For bug|	http)|TMS/SWDIO" try$i.log
                echo LATCHED; exit 0
            fi
            grep -E "Error" try$i.log | sort | uniq -c | head -3
            python3 pads.py 0x08 > /dev/null
            powered || { echo "power gone"; break; }
        done
    fi
    sleep 0.05
done
echo "gave up"
