init
halt 1000
catch { write_memory 0xE000ED0C 32 0x05FA0004 }
shutdown
