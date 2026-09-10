
function jit_get_error_string()
    local error_addr = jit_libc_error()
    local errno = jit_read64(error_addr)
    local str_addr = libc_strerror(errno)
    return "errno : " .. to_hex(errno) .. "\nerror : " .. read_null_terminated_string(str_addr)
end

function jit_send_recv_fd(target_fd, jit_src_sock, main_dest_sock)

    write32(smsg_control + 0,  20)         -- cmsg_len
    write32(smsg_control + 4,  SOL_SOCKET) -- cmsg_level
    write32(smsg_control + 8,  SCM_RIGHTS) -- cmsg_type
    write32(smsg_control + 12, 0)          -- padding
    write32(smsg_control + 16, target_fd)  -- the fd
    
    write8(smsg_data, 0)
    
    write64(smsg_iov, smsg_data)
    write64(smsg_iov + 8, 1)
    
    write64(smsg_msg + 0x10, smsg_iov)
    write32(smsg_msg + 0x18, 1)
    write64(smsg_msg + 0x20, smsg_control)
    write32(smsg_msg + 0x28, 20)
    
    local sendmsg_ret = jit_syscall.sendmsg(jit_src_sock, smsg_msg, 0)
    if (sendmsg_ret < 0) then
        send_notification("jit_syscall.sendmsg error: " .. jit_get_error_string())
        return -1
    end
    
    local received_fd = recv_fd(main_dest_sock)
    if (received_fd < 0) then
        send_notification("recv_fd error: " .. get_error_string())
        return -1
    end
    
    return received_fd
end

function jit_get_memory_info(addr, info_buf)
    local ret = jit_sceKernelVirtualQuery(addr, 0, info_buf, 0x48)
    if ret ~= 0 then
        send_notification("jit_sceKernelVirtualQuery error: " .. jit_get_error_string())
        return -1
    end

    local start = jit_read64(info_buf + 0x00)
    local ends  = jit_read64(info_buf + 0x08)
    local size  = ends - start

    return start, size
end
